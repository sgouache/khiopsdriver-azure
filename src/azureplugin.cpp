#ifdef __CYGWIN__
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "azureplugin.h"
#include "azureplugin_internal.h"
#include "contrib/matching.h"

#include <algorithm>
#include <assert.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <limits.h>
#include <memory>
#include <sstream>

#include "spdlog/spdlog.h"

// Include the necessary SDK headers
#include <azure/core.hpp>
#include <azure/storage/blobs.hpp>
// Include to support file shares
#include <azure/storage/files/shares.hpp>

#include <azure/identity/default_azure_credential.hpp>

#include <boost/uuid/uuid.hpp>            // uuid class
#include <boost/uuid/uuid_generators.hpp> // generators
#include <boost/uuid/uuid_io.hpp>         // streaming operators etc.

using namespace azureplugin;

constexpr const char* version = "0.1.0";
constexpr const char* driver_name = "Azure driver";
constexpr const char* driver_scheme = "https";
constexpr long long preferred_buffer_size = 4 * 1024 * 1024;

int bIsConnected = kFalse;

// Add appropriate using namespace directives
using namespace Azure::Storage;
using namespace Azure::Storage::Blobs;
using namespace Azure::Storage::Files::Shares;
using namespace Azure::Identity;

// Secrets should be stored & retrieved from secure locations such as Azure::KeyVault. For
// convenience and brevity of samples, the secrets are retrieved from environment variables.
std::string GetEndpointUrl()
{
	return std::getenv("AZURE_STORAGE_ACCOUNT_URL");
}
std::string GetAccountName()
{
	return std::getenv("AZURE_STORAGE_ACCOUNT_NAME");
}
std::string GetAccountKey()
{
	return std::getenv("AZURE_STORAGE_ACCOUNT_KEY");
}
std::string GetConnectionString()
{
	return std::getenv("AZURE_STORAGE_CONNECTION_STRING");
}

// Global bucket name
std::string globalBucketName;

// Last error
std::string lastError;

StreamVec<Reader> active_reader_handles;
StreamVec<Writer> active_writer_handles;

enum class Service
{
	UNKNOWN,
	BLOB,
	SHARE
};

template <typename T> struct DriverResult
{
	Azure::Response<T> response_;
	bool success_{false};

	DriverResult() : response_{{}, nullptr} // Azure::Response only has an explicit contructor
	{
	}

	explicit DriverResult(Azure::Response<T>&& response, bool success)
	    : response_{std::move(response)}, success_{success}
	{
	}

	template <typename U> DriverResult<U> ConvertFailureTo()
	{
		DriverResult<U> to;
		to.response_.RawResponse = std::move(response_.RawResponse);
		return to;
	}

	const T& GetValue() const
	{
		return response_.Value;
	}

	T& GetValue()
	{
		return response_.Value;
	}

	T&& TakeValue()
	{
		return std::move(response_.Value);
	}

	const decltype(response_.RawResponse)& GetRawResponse() const
	{
		return response_.RawResponse;
	}

	Azure::Core::Http::HttpStatusCode GetStatusCode() const
	{
		return GetRawResponse()->GetStatusCode();
	}

	const std::string& GetReasonPhrase() const
	{
		return GetRawResponse()->GetReasonPhrase();
	}

	explicit operator bool() const
	{
		return success_;
	}
};

#define KH_AZ_CONNECTION_ERROR(err_val)                                                                                \
	if (kFalse == bIsConnected)                                                                                    \
	{                                                                                                              \
		LogError("Error: driver not connected.");                                                              \
		return (err_val);                                                                                      \
	}

#define RETURN_STATUS(x) return std::move((x)).status();

#define RETURN_STATUS_ON_ERROR(x)                                                                                      \
	if (!(x))                                                                                                      \
	{                                                                                                              \
		RETURN_STATUS((x));                                                                                    \
	}

#define ERROR_ON_NULL_ARG(arg, err_val)                                                                                \
	if (!(arg))                                                                                                    \
	{                                                                                                              \
		std::ostringstream oss;                                                                                \
		oss << "Error passing null pointer to " << __func__;                                                   \
		LogError(oss.str());                                                                                   \
		return (err_val);                                                                                      \
	}

void LogError(const std::string& msg)
{
	lastError = msg;
	spdlog::error(lastError);
}

void LogException(const std::string& msg, const std::string& what)
{
	std::ostringstream oss;
	oss << msg << " " << what;
	LogError(oss.str());
}

#define LogBadStatus(a, b)

template <typename T> void LogBadResult(const DriverResult<T>& result, const std::string& msg)
{
	std::ostringstream oss;
	oss << msg << ": " << result.GetReasonPhrase();
	LogError(oss.str());
}

template <typename Stream> StreamIt<Stream> FindHandle(void* h, StreamVec<Stream>& handles)
{
	return std::find_if(handles.begin(), handles.end(),
			    [h](const StreamPtr<Stream>& act_h_ptr)
			    { return h == static_cast<void*>(act_h_ptr.get()); });
}

StreamIt<Reader> FindReader(void* h)
{
	return FindHandle<Reader>(h, active_reader_handles);
}

StreamIt<Writer> FindWriter(void* h)
{
	return FindHandle<Writer>(h, active_writer_handles);
}

template <typename Stream> void EraseRemove(StreamIt<Stream> pos, StreamVec<Stream>& handles)
{
	*pos = std::move(handles.back());
	handles.pop_back();
}

std::unique_ptr<Azure::Core::Http::RawResponse> MakeRawHttpResponsePtr(Azure::Core::Http::HttpStatusCode code,
								       const std::string& reason)
{
	return std::unique_ptr<Azure::Core::Http::RawResponse>(new Azure::Core::Http::RawResponse(1, 0, code, reason));
}

template <typename T>
DriverResult<T> MakeDriverHttpFailure(Azure::Core::Http::HttpStatusCode code, const std::string& reason)
{
	return DriverResult<T>{Azure::Response<T>(T{}, MakeRawHttpResponsePtr(code, reason)), false};
}

template <typename T> DriverResult<T> MakeDriverFailureFromException(const Azure::Storage::StorageException& e)
{
	return MakeDriverHttpFailure<T>(e.StatusCode, e.ReasonPhrase);
}

template <typename T> DriverResult<T> MakeDriverFailureFromException(const std::exception& e)
{
	return MakeDriverHttpFailure<T>(Azure::Core::Http::HttpStatusCode::None, e.what());
}

template <typename T> DriverResult<T> MakeDriverSuccess(T value)
{
	return DriverResult<T>{Azure::Response<T>(std::move(value), nullptr), true};
}

struct ParseUriResult
{
	Service service;
	std::string bucket;
	std::string object;
};

// Parses URI in the following forms:
//  - when accessing a real cloud service:
//      https://myaccount.blob.core.windows.net/mycontainer/myblob.txt
//  - when a storage emulator like Azurite is used, the URI will have a different form:
//    http[s]://127.0.0.1:10000/myaccount/mycontainer/myblob.txt
// Note: file service URIs e.g. https://myaccount.file.core.windows.net/myshare/myfolder/myfile.txt
//       are not supported at this time...
DriverResult<ParseUriResult> ParseAzureUri(const std::string& azure_uri)
{
	auto compare_suffix = [](const std::string& full, const std::string& suffix)
	{
		const size_t full_length = full.length();
		const size_t suffix_length = suffix.length();
		return (full_length >= suffix_length &&
			full.compare(full_length - suffix_length, suffix_length, suffix) == 0);
	};

	const Azure::Core::Url parsed_uri(azure_uri);

	if (parsed_uri.GetScheme() != "https" && parsed_uri.GetScheme() != "http")
	{
		return MakeDriverHttpFailure<ParseUriResult>(Azure::Core::Http::HttpStatusCode::BadRequest,
							     "Invalid Azure URI");
	}

	size_t bkt_pos = 0;
	size_t obj_pos = parsed_uri.GetPath().find('/');
	Service service = Service::UNKNOWN;

	const std::string host = parsed_uri.GetHost();
	constexpr auto az_domain = ".core.windows.net";

	if (compare_suffix(host, az_domain))
	{
		spdlog::debug("Provided URI is a production one.");

		constexpr auto blob_domain = ".blob.core.windows.net";
		constexpr auto file_domain = ".file.core.windows.net";

		if (compare_suffix(host, blob_domain))
		{
			spdlog::debug("Provided URI is a blob one.");
			service = Service::BLOB;
		}
		else if (compare_suffix(host, file_domain))
		{
			spdlog::debug("Provided URI is a file one.");
			service = Service::SHARE;
		}
	}
	else
	{
		spdlog::debug("Provided URI is a testing one.");
		service = Service::BLOB;
		bkt_pos = obj_pos + 1;
		obj_pos = parsed_uri.GetPath().find('/', bkt_pos);
	}

	if (obj_pos == std::string::npos)
	{
		return MakeDriverHttpFailure<ParseUriResult>(Azure::Core::Http::HttpStatusCode::BadRequest,
							     "Invalid Azure URI, missing object name: " + azure_uri);
	}

	ParseUriResult res{service, parsed_uri.GetPath().substr(bkt_pos, obj_pos - bkt_pos),
			   parsed_uri.GetPath().substr(obj_pos + 1)};

	return MakeDriverSuccess<ParseUriResult>(std::move(res));
}

DriverResult<ParseUriResult> GetServiceBucketAndObjectNames(const char* sFilePathName)
{
	auto maybe_parse_res = ParseAzureUri(sFilePathName);

	if (maybe_parse_res)
	{
		const ParseUriResult& val = maybe_parse_res.GetValue();
		spdlog::debug("Bucket: {}, Object: {}", val.bucket, val.object);
	}

	// std::cout << "Bucket: " << val.bucket << ", Object: " << val.object << "\n";
	/*
    if (!maybe_parse_res)
    {
        return maybe_parse_res;
    }

    // fallback to default bucket if bucket empty
    if (maybe_parse_res->bucket.empty())
    {
        if (globalBucketName.empty())
        {
            maybe_parse_res = gc::Status{gc::StatusCode::kInternal, "No bucket specified and GCS_BUCKET_NAME is not set!"};
        }
        else
        {
            maybe_parse_res->bucket = globalBucketName;
        }
    }
    */
	return maybe_parse_res;
}

std::string ToLower(const std::string& str)
{
	std::string low{str};
	const size_t cnt = low.length();
	for (size_t i = 0; i < cnt; i++)
	{
		low[i] = static_cast<char>(std::tolower(
		    static_cast<unsigned char>(low[i]))); // see https://en.cppreference.com/w/cpp/string/byte/tolower
	}
	return low;
}

std::string GetEnvironmentVariableOrDefault(const std::string& variable_name, const std::string& default_value)
{
	char* value = std::getenv(variable_name.c_str());

	if (value && std::strlen(value) > 0)
	{
		return value;
	}

	const std::string low_key = ToLower(variable_name);
	if (low_key.find("token") || low_key.find("password") || low_key.find("key") || low_key.find("secret"))
	{
		spdlog::debug("No {} specified, using **REDACTED** as default.", variable_name);
	}
	else
	{
		spdlog::debug("No {} specified, using '{}' as default.", variable_name, default_value);
	}

	return default_value;
}

std::string GetConnectionStringFromEnv()
{
	// TODO Should allow different auth options like described in: https://learn.microsoft.com/en-us/azure/storage/blobs/authorize-data-operations-cli
	return GetEnvironmentVariableOrDefault(
	    "AZURE_STORAGE_CONNECTION_STRING",
	    "DefaultEndpointsProtocol=http;AccountName=devstoreaccount1;AccountKey="
	    "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/"
	    "KBHBeksoGMGw==;BlobEndpoint=http://127.0.0.1:10000/devstoreaccount1;");
}

template <typename ServiceClientType> ServiceClientType GetServiceClient()
{
	return ServiceClientType::CreateFromConnectionString(GetConnectionStringFromEnv());
}

bool WillSizeCountProductOverflow(size_t size, size_t count)
{
	constexpr size_t max_prod_usable{static_cast<size_t>(std::numeric_limits<tOffset>::max())};
	return (max_prod_usable / size < count || max_prod_usable / count < size);
}

/*
// pre condition: stream is of a writing type. do not call otherwise.
gc::Status CloseWriterStream(Handle &stream)
{
    gc::StatusOr<gcs::ObjectMetadata> maybe_meta;
    std::ostringstream err_msg_os;

    // close the stream to flush all remaining bytes in the put area
    auto &writer = stream.GetWriter().writer_;
    writer.Close();
    maybe_meta = writer.metadata();
    if (!maybe_meta)
    {
        err_msg_os << "Error during upload";
    }
    else if (HandleType::kAppend == stream.type)
    {
        // the tmp file is valid and ready for composition with the source
        const auto &writer_h = stream.GetWriter();
        const std::string &bucket = writer_h.bucketname_;
        const std::string &append_source = writer_h.filename_;
        const std::string &dest = writer_h.append_target_;
        std::vector<gcs::ComposeSourceObject> source_objects = {{dest, {}, {}}, {append_source, {}, {}}};
        maybe_meta = client.ComposeObject(bucket, std::move(source_objects), dest);

        // whatever happened, delete the tmp file
        gc::Status delete_status = client.DeleteObject(bucket, append_source);

        // TODO: what to do with an error on Delete?
        (void)delete_status;

        // if composition failed, nothing is written, the source did not change. signal it
        if (!maybe_meta)
        {
            err_msg_os << "Error while uploading the data to append";
        }
    }

    if (maybe_meta)
    {
        return {};
    }

    const gc::Status &status = maybe_meta.status();
    err_msg_os << ": " << maybe_meta.status().message();
    return gc::Status{status.code(), err_msg_os.str()};
}
*/
// Implementation of driver functions

// Get from a container a list of blobs matching a name pattern.
// To get a limited list of blobs to filter per request, the request includes a well defined
// prefix contained in the pattern
using BlobItems = std::vector<Azure::Storage::Blobs::Models::BlobItem>;
DriverResult<BlobItems> FilterList(const std::string& bucket, const std::string& pattern,
				   size_t pattern_1st_sp_char_pos)
{
	using value_t = BlobItems;

	std::vector<Azure::Storage::Blobs::Models::BlobItem> res;

	auto container_client = GetServiceClient<BlobServiceClient>().GetBlobContainerClient(bucket);
	ListBlobsOptions options;
	options.Prefix = pattern.substr(0, pattern_1st_sp_char_pos);
	try
	{
		for (auto blobs_page = container_client.ListBlobs(options); blobs_page.HasPage();
		     blobs_page.MoveToNextPage())
		{
			// filter blobs by match with pattern
			for (auto& item : blobs_page.Blobs)
			{
				if (!item.IsDeleted && utils::gitignore_glob_match(item.Name, pattern))
				{
					res.emplace_back(std::move(item));
				}
			}
		}
		if (res.empty())
		{
			return MakeDriverHttpFailure<value_t>(Azure::Core::Http::HttpStatusCode::NotFound,
							      "No blob matching pattern in container.");
		}
		return MakeDriverSuccess<value_t>(std::move(res));
	}
	catch (const Azure::Storage::StorageException& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}
	catch (const std::exception& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}
}

const char* driver_getDriverName()
{
	return driver_name;
}

const char* driver_getVersion()
{
	return version;
}

const char* driver_getScheme()
{
	return driver_scheme;
}

int driver_isReadOnly()
{
	return kFalse;
}

int driver_connect()
{
	const std::string loglevel = GetEnvironmentVariableOrDefault("AZURE_DRIVER_LOGLEVEL", "info");
	if (loglevel == "debug")
		spdlog::set_level(spdlog::level::debug);
	else if (loglevel == "trace")
		spdlog::set_level(spdlog::level::trace);
	else
		spdlog::set_level(spdlog::level::info);

	spdlog::debug("Connect {}", loglevel);

	// Initialize variables from environment
	globalBucketName = GetEnvironmentVariableOrDefault("AZURE_BUCKET_NAME", "");

	// Tester la connexion
	const auto blobServiceClient = GetServiceClient<BlobServiceClient>();
	try
	{
		blobServiceClient.GetProperties();
		spdlog::debug("Connexion valide.");
		bIsConnected = kTrue;
		return kSuccess;
	}
	catch (const std::exception& e)
	{
		LogException("Connection error.", e.what());
		return kFailure;
	}
}

int driver_disconnect()
{
	// clear handles
	active_reader_handles.clear();
	active_writer_handles.clear();

	// manage internal state
	bIsConnected = kFalse;
	return kSuccess;
}

int driver_isConnected()
{
	return bIsConnected;
}

long long int driver_getSystemPreferredBufferSize()
{
	return preferred_buffer_size; // 4 Mo
}

int driver_exist(const char* filename)
{
	KH_AZ_CONNECTION_ERROR(kFalse);

	ERROR_ON_NULL_ARG(filename, kFalse);

	spdlog::debug("exist {}", filename);

	std::string file_uri = filename;
	spdlog::debug("exist file_uri {}", file_uri);
	spdlog::debug("exist last char {}", file_uri.back());

	if (file_uri.back() == '/')
	{
		return driver_dirExists(filename);
	}
	else
	{
		return driver_fileExists(filename);
	}
}

#define RETURN_ON_ERROR(driver_result, msg, err_val)                                                                   \
	if (!(driver_result))                                                                                          \
	{                                                                                                              \
		LogBadResult((driver_result), (msg));                                                                  \
		return (err_val);                                                                                      \
	}

#define ERROR_ON_NAMES(names_result, err_val) RETURN_ON_ERROR((names_result), "Error parsing URL", (err_val))

Azure::Nullable<size_t> FindPatternSpecialChar(const std::string& pattern)
{
	spdlog::debug("Parse multifile pattern {}", pattern);

	constexpr auto special_chars = "*?![^";

	size_t from_offset = 0;
	size_t found_at = pattern.find_first_of(special_chars, from_offset);
	while (found_at != std::string::npos)
	{
		const char found = pattern[found_at];
		spdlog::debug("special char {} found at {}", found, found_at);

		if (found_at > 0 && pattern[found_at - 1] == '\\')
		{
			spdlog::debug("preceded by a \\, so not so special");
			from_offset = found_at + 1;
			found_at = pattern.find_first_of(special_chars, from_offset);
		}
		else
		{
			spdlog::debug("not preceded by a \\, so really a special char");
			break;
		}
	}
	return found_at != std::string::npos ? found_at : Azure::Nullable<size_t>{};
}

int driver_fileExists(const char* sFilePathName)
{
	KH_AZ_CONNECTION_ERROR(kFalse);

	ERROR_ON_NULL_ARG(sFilePathName, kFalse);

	spdlog::debug("fileExist {}", sFilePathName);

	auto maybe_parsed_names = ParseAzureUri(sFilePathName);
	if (!maybe_parsed_names)
	{
		std::ostringstream oss;
		oss << "Error while parsing Uri. " << maybe_parsed_names.GetReasonPhrase();
		LogError(oss.str());
	}

	const auto& val = maybe_parsed_names.GetValue();
	if (Service::BLOB != val.service)
	{
		LogError("Error checking blob's existence: not a URL of a blob service.");
		return kFalse;
	}

	const auto pattern_1st_sp_char_pos = FindPatternSpecialChar(val.object);
	if (!pattern_1st_sp_char_pos)
	{
		const auto& blob_client =
		    GetServiceClient<BlobServiceClient>().GetBlobContainerClient(val.bucket).GetBlobClient(val.object);
		try
		{
			// GetProperties throws in case of error, even if the error is NotFound.
			blob_client.GetProperties();
			spdlog::debug("file {} exists.", sFilePathName);
			return kTrue;
		}
		catch (const Azure::Storage::StorageException& e)
		{
			if (e.StatusCode == Azure::Core::Http::HttpStatusCode::NotFound)
			{
				spdlog::debug("File not found. {}", e.what());
			}
			else
			{
				LogException("Error while checking file's presence.", e.what());
			}
			return kFalse;
		}
		catch (const std::exception& e)
		{
			LogException("Error while checking file's presence.", e.what());
			return kFalse;
		}
	}

	const auto filter_res = FilterList(val.bucket, val.object, *pattern_1st_sp_char_pos);
	if (!filter_res)
	{
		if (spdlog::get_level() >= spdlog::level::debug)
		{
			if (Azure::Core::Http::HttpStatusCode::NotFound == filter_res.GetStatusCode())
			{
				spdlog::debug(filter_res.GetReasonPhrase());
			}
			else
			{
				LogBadResult<BlobItems>(filter_res, "Error while listing blobs in container.");
			}
		}
		return kFalse;
	}

	return kTrue;
}

int driver_dirExists(const char* sFilePathName)
{
	KH_AZ_CONNECTION_ERROR(kFalse);

	ERROR_ON_NULL_ARG(sFilePathName, kFalse);

	spdlog::debug("dirExist {}", sFilePathName);
	return kTrue;
}

DownloadBlobToOptions MakeDlBlobOptions(int64_t range_start, Azure::Nullable<int64_t> range_length = {})
{
	Azure::Core::Http::HttpRange range{range_start, range_length};
	DownloadBlobToOptions opt;
	opt.Range = std::move(range);
	return opt;
}

// this function rethrows the exception thrown by Azure's API if an error occurs.
size_t ReadPart(const BlobClient& blob_client, std::vector<uint8_t>& dest, int64_t offset)
{
	const size_t to_read = dest.size();
	const auto dl_options = MakeDlBlobOptions(offset, static_cast<int64_t>(to_read));
	const auto dl_res = blob_client.DownloadTo(dest.data(), to_read, dl_options);
	return static_cast<size_t>(*(dl_res.Value.ContentRange.Length));
}

std::vector<uint8_t> ReadPart(const BlobClient& blob_client, int64_t offset, size_t to_read)
{
	std::vector<uint8_t> dest(to_read);
	const size_t bytes_read = ReadPart(blob_client, dest, offset);
	if (bytes_read < to_read)
	{
		dest.resize(bytes_read);
	}
	return dest;
}

Azure::Nullable<std::vector<uint8_t>> FindHeader(const BlobClient& blob_client)
{
	constexpr size_t block_size{10 * 1024 * 1024};
	std::vector<uint8_t> header(block_size);

	// read by blocks until new line char is found or end of stream
	int64_t bytes_read = 0;

	bool found = false;
	bool exhausted = false;

	while (!exhausted)
	{
		const auto new_data_start = header.data() + bytes_read;
		const auto dl_options = MakeDlBlobOptions(bytes_read, block_size);
		const auto dl_res = blob_client.DownloadTo(new_data_start, block_size, dl_options);
		const auto dl_bytes_read = *(dl_res.Value.ContentRange.Length);

		const auto search_end = new_data_start + dl_bytes_read;
		const auto new_line_pos = std::find(new_data_start, search_end, static_cast<uint8_t>('\n'));

		if (new_line_pos != search_end)
		{
			// found it!
			// trim the vector containing the header
			found = true;
			header.resize(static_cast<size_t>(std::distance(header.data(), new_line_pos) + 1u));
			break;
		}

		bytes_read += dl_bytes_read;

		exhausted = bytes_read == dl_res.Value.BlobSize;

		if (!exhausted)
		{
			header.resize(header.size() + block_size);
		}
	}

	return found ? std::move(header) : Azure::Nullable<std::vector<uint8_t>>{};
}

bool IsSameHeader(const BlobContainerClient& container_client, const Blobs::Models::BlobItem& blob_item,
		  const std::vector<uint8_t>& header, std::vector<uint8_t>& part_buffer)
{
	const auto blob_client = container_client.GetBlobClient(blob_item.Name);
	ReadPart(blob_client, part_buffer, 0);
	return part_buffer == header;
}

DriverResult<long long> GetFileSize(const ParseUriResult& parsed_names)
{
	using value_t = long long;

	const auto container_client = GetServiceClient<BlobServiceClient>().GetBlobContainerClient(parsed_names.bucket);

	// if single file, get it, else multifile pattern, list files
	const std::string& object = parsed_names.object;
	const auto pattern_1st_sp_char_pos = FindPatternSpecialChar(object);

	if (!pattern_1st_sp_char_pos)
	{
		// get the property directly
		const auto blob_client = container_client.GetBlobClient(object);
		try
		{
			const auto props = blob_client.GetProperties();
			value_t result = static_cast<value_t>(props.Value.BlobSize);
			return MakeDriverSuccess<value_t>(result);
		}
		catch (const Azure::Storage::StorageException& e)
		{
			return MakeDriverFailureFromException<value_t>(e);
		}
		catch (const std::exception& e)
		{
			return MakeDriverFailureFromException<value_t>(e);
		}
	}

	//multifile, list the files corresponding to the pattern
	auto filter_res = FilterList(parsed_names.bucket, object, *pattern_1st_sp_char_pos);
	if (!filter_res)
	{
		return filter_res.ConvertFailureTo<value_t>();
	}

	const auto& blobs_list = filter_res.GetValue();

	// if single file, short treatment
	if (blobs_list.size() == 1)
	{
		return MakeDriverSuccess(static_cast<long long>(blobs_list[0].BlobSize));
	}

	// several files
	// get the sizes and read the headers to identify common headers if any
	// first file
	const auto first_blob_client = container_client.GetBlobClient(blobs_list[0].Name);
	try
	{
		const auto maybe_header = FindHeader(first_blob_client);

		if (!maybe_header)
		{
			return MakeDriverHttpFailure<long long>(Azure::Core::Http::HttpStatusCode::None,
								"Error while reading header of first file.");
		}

		const std::vector<uint8_t>& header = *maybe_header;
		const size_t header_size = header.size();

		// next files
		const size_t blob_count = blobs_list.size();
		long long total_size = blobs_list[0].BlobSize;
		bool same_headers = true;
		std::vector<uint8_t> part_buffer(header_size);

		for (size_t i = 1; i < blob_count; i++)
		{
			const auto& blob_item = blobs_list[i];
			total_size += static_cast<long long>(blob_item.BlobSize);

			// read the beginning of the file to check header if still necessary
			if (same_headers)
			{
				same_headers = IsSameHeader(container_client, blob_item, header, part_buffer);
			}
		}

		long long result =
		    same_headers ? total_size - static_cast<long long>((blob_count - 1) * header_size) : total_size;

		return MakeDriverSuccess<long long>(result);
	}
	catch (const Azure::Storage::StorageException& e)
	{
		return MakeDriverFailureFromException<long long>(e);
	}
	catch (const std::exception& e)
	{
		return MakeDriverFailureFromException<long long>(e);
	}
}

long long int driver_getFileSize(const char* filename)
{
	KH_AZ_CONNECTION_ERROR(kBadSize);

	ERROR_ON_NULL_ARG(filename, kBadSize);

	spdlog::debug("getFileSize {}", filename);

	const auto maybe_parsed_names = ParseAzureUri(filename);
	ERROR_ON_NAMES(maybe_parsed_names, kBadSize);

	const auto& parsed_names = maybe_parsed_names.GetValue();

	if (Service::BLOB != parsed_names.service)
	{
		LogError("Functionality not implemented for this type of service.");
		return kBadSize;
	}

	// if (val.service == SHARE) {
	//     auto props = GetShareServiceClient().GetShareClient(val.bucket)
	//             .GetRootDirectoryClient().GetFileClient(val.object)
	//             .GetProperties();
	//     return props.Value.FileSize;

	const auto res = GetFileSize(parsed_names);
	if (!res)
	{
		LogBadResult(res, "Error while getting file size.");
		return kBadSize;
	}

	return res.GetValue();
}

DriverResult<long long> ReadBytesInFile(Reader& multifile, unsigned char* buffer, tOffset to_read)
{
	// Start at first usable file chunk
	// Advance through file chunks, advancing buffer pointer
	// Until last requested byte was read
	// Or error occured

	const auto& cumul_sizes = multifile.cumulativeSize_;
	const tOffset common_header_length = multifile.commonHeaderLength_;
	const auto& filenames = multifile.filenames_;
	unsigned char* buffer_pos = buffer;
	tOffset& offset = multifile.offset_;
	const tOffset offset_bak = offset; // in case of irrecoverable error, leave the multifile in its starting state

	auto recover_offset = [&]() { offset = offset_bak; };

	auto read_range_and_update =
	    [&](const BlobContainerClient& container, const std::string& filename, tOffset start, tOffset length)
	{
		const auto blob_client = container.GetBlobClient(filename);
		const auto dl_options = MakeDlBlobOptions(static_cast<int64_t>(start), static_cast<int64_t>(length));
		blob_client.DownloadTo(static_cast<uint8_t*>(buffer_pos), static_cast<size_t>(length), dl_options);

		offset += length;
		buffer_pos += length;
		to_read -= length;
	};

	// Lookup item containing initial bytes at requested offset
	auto greater_than_offset_it = std::upper_bound(cumul_sizes.begin(), cumul_sizes.end(), offset);
	size_t idx = static_cast<size_t>(std::distance(cumul_sizes.begin(), greater_than_offset_it));

	spdlog::debug("Use item {} to read @ {} (end = {})", idx, offset, *greater_than_offset_it);

	const auto container_client =
	    GetServiceClient<BlobServiceClient>().GetBlobContainerClient(multifile.bucketname_);

	// first file read
	const tOffset file_start = (idx == 0) ? offset : offset - cumul_sizes[idx - 1] + common_header_length;
	const tOffset read_length = std::min(to_read, cumul_sizes[idx] - offset);

	try
	{
		read_range_and_update(container_client, filenames[idx], file_start, read_length);
	}
	catch (const StorageException& e)
	{
		recover_offset();
		return MakeDriverFailureFromException<long long>(e);
	}
	catch (const std::exception& e)
	{
		recover_offset();
		return MakeDriverFailureFromException<long long>(e);
	}

	// continue with the next files
	while (to_read)
	{
		// read the missing bytes in the next files as necessary
		idx++;
		const tOffset start = common_header_length;
		const tOffset length = std::min(to_read, cumul_sizes[idx] - cumul_sizes[idx - 1]);
		try
		{
			read_range_and_update(container_client, filenames[idx], start, length);
		}
		catch (const StorageException& e)
		{
			recover_offset();
			return MakeDriverFailureFromException<long long>(e);
		}
		catch (const std::exception& e)
		{
			recover_offset();
			return MakeDriverFailureFromException<long long>(e);
		}
	}

	return MakeDriverSuccess<long long>(to_read);
}

enum class ReaderMode
{
	kNone
};

DriverResult<ReaderPtr> MakeReaderPtr(std::string bucket, std::string object, ReaderMode mode)
{
	using value_t = ReaderPtr;

	(void)mode; //unused, silence warnings

	auto make_simple_reader = [](std::string&& bucket, std::string&& object, std::string&& filename,
				     long long blob_size) -> ReaderPtr
	{
		std::vector<std::string> filenames;
		filenames.emplace_back(std::move(filename));
		std::vector<long long> cumulative_sizes = {blob_size};
		return std::make_unique<Reader>(std::move(bucket), std::move(object), 0, 0, std::move(filenames),
						std::move(cumulative_sizes));
	};

	const auto container_client = GetServiceClient<BlobServiceClient>().GetBlobContainerClient(bucket);

	// if single file, get it, else multifile pattern, list files
	const auto pattern_1st_sp_char_pos = FindPatternSpecialChar(object);

	if (!pattern_1st_sp_char_pos)
	{
		auto blob_client = container_client.GetBlobClient(object);
		try
		{
			const auto props_response = blob_client.GetProperties();
			std::string filename = object;
			return MakeDriverSuccess<value_t>(
			    make_simple_reader(std::move(bucket), std::move(object), std::move(filename),
					       static_cast<long long>(props_response.Value.BlobSize)));
		}
		catch (const Azure::Storage::StorageException& e)
		{
			return MakeDriverHttpFailure<value_t>(e.StatusCode, e.ReasonPhrase);
		}
		catch (const std::exception& e)
		{
			return MakeDriverHttpFailure<value_t>(Azure::Core::Http::HttpStatusCode::None, e.what());
		}
	}

	//multifile, list the files corresponding to the pattern
	auto filter_res = FilterList(bucket, object, *pattern_1st_sp_char_pos);
	if (!filter_res)
	{
		return filter_res.ConvertFailureTo<value_t>();
	}

	auto& blobs_list = filter_res.GetValue();

	// if single file, short treatment
	if (blobs_list.size() == 1)
	{
		auto& item = blobs_list[0];
		return MakeDriverSuccess<value_t>(make_simple_reader(
		    std::move(bucket), std::move(object), std::move(item.Name), static_cast<long long>(item.BlobSize)));
	}

	// several files, gather data about names and sizes
	const size_t blob_count = blobs_list.size();
	std::vector<std::string> filenames;
	filenames.reserve(blob_count);
	std::vector<long long> cumulativeSize;
	cumulativeSize.reserve(blob_count);

	const auto& first_blob = blobs_list[0];
	const auto first_blob_client = container_client.GetBlobClient(first_blob.Name);
	try
	{
		const auto maybe_header = FindHeader(first_blob_client);

		if (!maybe_header)
		{
			return MakeDriverHttpFailure<value_t>(Azure::Core::Http::HttpStatusCode::None,
							      "Error while reading header of first file.");
		}

		const std::vector<uint8_t>& header = *maybe_header;
		const size_t header_size = header.size();
		filenames.emplace_back(first_blob.Name);
		cumulativeSize.push_back(static_cast<long long>(blobs_list[0].BlobSize));

		// next files
		bool same_headers = true;
		std::vector<uint8_t> part_buffer(header_size);

		for (size_t i = 1; i < blob_count; i++)
		{
			const auto& curr_blob_item = blobs_list[i];
			filenames.emplace_back(curr_blob_item.Name);
			cumulativeSize.push_back(cumulativeSize.back() +
						 static_cast<long long>(curr_blob_item.BlobSize));

			// read the beginning of the file to check header if still necessary
			if (same_headers)
			{
				same_headers = IsSameHeader(container_client, curr_blob_item, header, part_buffer);
			}
		}

		if (same_headers)
		{
			// update cumulativeSize
			for (size_t i = 1; i < blob_count; i++)
			{
				cumulativeSize[i] -= static_cast<long long>(i * header_size);
			}
		}

		return MakeDriverSuccess<value_t>(std::make_unique<Reader>(
		    std::move(bucket), std::move(object), 0, static_cast<long long>(header_size), std::move(filenames),
		    std::move(cumulativeSize)));
	}
	catch (const Azure::Storage::StorageException& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}
	catch (const std::exception& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}
}

enum class WriterMode
{
	kWrite,
	kAppend
};

DriverResult<WriterPtr> MakeWriterPtr(std::string bucket, std::string object, WriterMode mode)
{
	using value_t = WriterPtr;

	auto append_client = AppendBlobClient::CreateFromConnectionString(GetConnectionStringFromEnv(), bucket, object);

	try
	{
		switch (mode)
		{
		case WriterMode::kWrite:
		{
			const auto response = append_client.Create();
			if (!response.Value.Created)
			{
				const auto& raw_response = *(response.RawResponse);
				return MakeDriverHttpFailure<WriterPtr>(raw_response.GetStatusCode(),
									raw_response.GetReasonPhrase());
			}
			break;
		}
		case WriterMode::kAppend:
		{
			const auto response = append_client.CreateIfNotExists();
			if (!response.Value.Created)
			{
				spdlog::debug("File already exists, no creation needed before appending.");
			}
			break;
		}
		}
	}
	catch (const StorageException& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}
	catch (const std::exception& e)
	{
		return MakeDriverFailureFromException<value_t>(e);
	}

	auto writer_ptr = std::make_unique<Writer>(std::move(bucket), std::move(object), std::move(append_client));
	return MakeDriverSuccess<value_t>(std::move(writer_ptr));
}

// This template is only here to get specialized
template <typename Stream> Stream* PushBackHandle(StreamPtr<Stream>&& stream_ptr, StreamVec<Stream>& handles)
{
	Stream* res = stream_ptr.get();
	handles.push_back(std::move(stream_ptr));
	return res;
}

template <typename Stream, typename Mode>
DriverResult<Stream*>
RegisterStream(std::function<DriverResult<StreamPtr<Stream>>(std::string, std::string, Mode)> MakeStreamPtr, Mode mode,
	       std::string&& bucket, std::string&& object, StreamVec<Stream>& streams)
{
	auto maybe_stream_ptr = MakeStreamPtr(std::move(bucket), std::move(object), mode);
	if (!maybe_stream_ptr)
	{
		return maybe_stream_ptr.template ConvertFailureTo<Stream*>();
	}
	return MakeDriverSuccess<Stream*>(PushBackHandle(maybe_stream_ptr.TakeValue(), streams));
}

DriverResult<Reader*> RegisterReader(std::string&& bucket, std::string&& object)
{
	return RegisterStream<Reader, ReaderMode>(MakeReaderPtr, ReaderMode::kNone, std::move(bucket),
						  std::move(object), active_reader_handles);
}

DriverResult<Writer*> RegisterWriter(std::string&& bucket, std::string&& object, WriterMode mode)
{
	return RegisterStream<Writer, WriterMode>(MakeWriterPtr, mode, std::move(bucket), std::move(object),
						  active_writer_handles);
}

void* driver_fopen(const char* filename, char mode)
{
	KH_AZ_CONNECTION_ERROR(nullptr);

	ERROR_ON_NULL_ARG(filename, nullptr);

	spdlog::debug("fopen {} {}", filename, mode);

	auto name_parsing_result = ParseAzureUri(filename);
	ERROR_ON_NAMES(name_parsing_result, nullptr);

	auto& parsed_names = name_parsing_result.GetValue();

	switch (mode)
	{
	case 'r':
	{
		auto register_res = RegisterReader(std::move(parsed_names.bucket), std::move(parsed_names.object));
		if (!register_res)
		{
			LogBadResult(register_res, "Error while opening reader stream.");
			return nullptr;
		}
		return register_res.GetValue();
	}
	case 'w':
	{
		auto register_res =
		    RegisterWriter(std::move(parsed_names.bucket), std::move(parsed_names.object), WriterMode::kWrite);
		if (!register_res)
		{
			LogBadResult(register_res, "Error while opening writer stream.");
			return nullptr;
		}
		return register_res.GetValue();
	}
	case 'a':
	{
		std::string target = std::move(parsed_names.object);
		// determine if object is a multifile
		const auto pattern_1st_sp_char_pos = FindPatternSpecialChar(target);
		if (pattern_1st_sp_char_pos)
		{
			// filter the present blobs and pick the last file as target
			const auto container_client = BlobContainerClient::CreateFromConnectionString(
			    GetConnectionStringFromEnv(), parsed_names.bucket);
			auto filter_res = FilterList(parsed_names.bucket, target, *pattern_1st_sp_char_pos);
			if (!filter_res)
			{
				LogBadResult(filter_res, "Error while opening stream in append mode.");
				return nullptr;
			}
			target = std::move(filter_res.GetValue().back().Name);
		}

		// open the stream
		auto register_res =
		    RegisterWriter(std::move(parsed_names.bucket), std::move(target), WriterMode::kAppend);
		if (!register_res)
		{
			LogBadResult(register_res, "Error while opening stream in append mode.");
			return nullptr;
		}
		return register_res.GetValue();
	}
	default:
		LogError("Invalid open mode: " + mode);
		return nullptr;
	}
}

// #define ERROR_NO_STREAM(handle_it, errval)                                                                             \
// 	if ((handle_it) == active_handles.end())                                                                       \
// 	{                                                                                                              \
// 		LogError("Cannot identify stream");                                                                    \
// 		return (errval);                                                                                       \
// 	}

int driver_fclose(void* stream)
{
	assert(driver_isConnected());

	ERROR_ON_NULL_ARG(stream, kCloseEOF);

	spdlog::debug("fclose {}", (void*)stream);

	const auto reader_stream_it = FindReader(stream);
	if (reader_stream_it != active_reader_handles.end())
	{
		EraseRemove<Reader>(reader_stream_it, active_reader_handles);
		return kCloseSuccess;
	}

	const auto writer_stream_it = FindWriter(stream);
	if (writer_stream_it != active_writer_handles.end())
	{
		EraseRemove<Writer>(writer_stream_it, active_writer_handles);
		return kCloseSuccess;
	}

	LogError("Cannot identify stream.");
	return kCloseEOF;
}

int driver_fseek(void* stream, long long int offset, int whence)
{
	KH_AZ_CONNECTION_ERROR(kBadSize);

	ERROR_ON_NULL_ARG(stream, kBadSize);

	spdlog::debug("fseek {} {} {}", stream, offset, whence);

	// confirm stream's presence
	auto reader_ptr_it = FindReader(stream);
	if (reader_ptr_it == active_reader_handles.end())
	{
		LogError("Cannot identify stream as a reader stream.");
		return kBadSize;
	}

	Reader& reader = **reader_ptr_it;

	constexpr long long max_val = std::numeric_limits<long long>::max();

	tOffset computed_offset{0};

	switch (whence)
	{
	case std::ios::beg:
		computed_offset = offset;
		break;
	case std::ios::cur:
		if (offset > max_val - reader.offset_)
		{
			LogError("Signed overflow prevented");
			return kBadSize;
		}
		computed_offset = reader.offset_ + offset;
		break;
	case std::ios::end:
		if (reader.total_size_ > 0)
		{
			long long minus1 = reader.total_size_ - 1;
			if (offset > max_val - minus1)
			{
				LogError("Signed overflow prevented");
				return kBadSize;
			}
		}
		if ((offset == std::numeric_limits<long long>::min()) && (reader.total_size_ == 0))
		{
			LogError("Signed overflow prevented");
			return kBadSize;
		}

		computed_offset = (reader.total_size_ == 0) ? offset : reader.total_size_ - 1 + offset;
		break;
	default:
		LogError("Invalid seek mode " + std::to_string(whence));
		return kBadSize;
	}

	if (computed_offset < 0)
	{
		LogError("Invalid seek offset " + std::to_string(computed_offset));
		return kBadSize;
	}
	reader.offset_ = computed_offset;
	return 0;
}

const char* driver_getlasterror()
{
	spdlog::debug("getlasterror");

	if (!lastError.empty())
	{
		return lastError.c_str();
	}
	return NULL;
}

long long int driver_fread(void* ptr, size_t size, size_t count, void* stream)
{
	KH_AZ_CONNECTION_ERROR(kBadSize);

	ERROR_ON_NULL_ARG(stream, kBadSize);
	ERROR_ON_NULL_ARG(ptr, kBadSize);

	if (0 == size)
	{
		LogError("Error passing size of 0");
		return kBadSize;
	}

	spdlog::debug("fread {} {} {} {}", ptr, size, count, stream);

	// confirm stream's presence
	const auto& reader_ptr_it = FindReader(stream);
	if (reader_ptr_it == active_reader_handles.end())
	{
		LogError("Cannot identify stream as a reader stream.");
		return kBadSize;
	}

	// fast exit for 0 read
	if (0 == count)
	{
		return 0;
	}

	// prevent overflow
	if (WillSizeCountProductOverflow(size, count))
	{
		LogError("product size * count is too large, would overflow");
		return kBadSize;
	}
	tOffset to_read{static_cast<tOffset>(size * count)};

	Reader& reader = **reader_ptr_it;
	const tOffset offset = reader.offset_;
	if (offset > std::numeric_limits<long long>::max() - to_read)
	{
		LogError("signed overflow prevented on reading attempt");
		return kBadSize;
	}
	// end of overflow prevention

	// special case: if offset >= total_size, error if not 0 byte required. 0 byte required is already done above
	const tOffset total_size = reader.total_size_;
	if (offset >= total_size)
	{
		LogError("Error trying to read more bytes while already out of bounds");
		return kBadSize;
	}

	// normal cases
	if (offset + to_read > total_size)
	{
		to_read = total_size - offset;
		spdlog::debug("offset {}, req len {} exceeds file size ({}) -> reducing len to {}", offset, to_read,
			      total_size, to_read);
	}
	else
	{
		spdlog::debug("offset = {} to_read = {}", offset, to_read);
	}

	const auto read_res = ReadBytesInFile(reader, reinterpret_cast<unsigned char*>(ptr), to_read);
	if (!read_res)
	{
		LogBadResult(read_res, "Error while reading from file.");
		return kBadSize;
	}

	return to_read;
}

long long int driver_fwrite(const void* ptr, size_t size, size_t count, void* stream)
{
	KH_AZ_CONNECTION_ERROR(kBadSize);

	ERROR_ON_NULL_ARG(stream, kBadSize);
	ERROR_ON_NULL_ARG(ptr, kBadSize);

	if (0 == size)
	{
		LogError("Error passing size 0 to fwrite");
		return kBadSize;
	}

	spdlog::debug("fwrite {} {} {} {}", ptr, size, count, stream);

	const auto& writer_ptr_it = FindWriter(stream);
	if (writer_ptr_it == active_writer_handles.end())
	{
		LogError("Cannot identify stream as a writer stream.");
		return kBadSize;
	}

	// fast exit for 0
	if (0 == count)
	{
		return 0;
	}

	// prevent integer overflow
	if (WillSizeCountProductOverflow(size, count))
	{
		LogError("Error on write: product size * count is too large, would overflow");
		return kBadSize;
	}

	// stage a block containing the data from the buffer
	auto& writer = **writer_ptr_it;
	const size_t to_write = size * count;
	Azure::Core::IO::MemoryBodyStream streambuf(reinterpret_cast<const uint8_t*>(ptr), to_write);

	try
	{
		writer.client_.AppendBlock(streambuf);
		return static_cast<long long>(to_write);
	}
	catch (const StorageException& e)
	{
		LogException("Error while writing data.", e.what());
	}
	catch (const std::exception& e)
	{
		LogException("Error while writing data.", e.what());
	}

	return kBadSize;
}

int driver_fflush(void* stream)
{
	KH_AZ_CONNECTION_ERROR(-1);

	ERROR_ON_NULL_ARG(stream, -1);

	const auto& writer_ptr_it = FindWriter(stream);
	if (writer_ptr_it == active_writer_handles.end())
	{
		LogError("Cannot identify stream as a writer stream.");
		return -1;
	}

	return 0;
}

int driver_remove(const char* filename)
{
	KH_AZ_CONNECTION_ERROR(kFailure);

	ERROR_ON_NULL_ARG(filename, kFailure);

	spdlog::debug("remove {}", filename);

	const auto name_parsing_result = ParseAzureUri(filename);
	ERROR_ON_NAMES(name_parsing_result, kFailure);
	const auto& names = name_parsing_result.GetValue();

	spdlog::info("Deleting blob: {}.", names.object);

	// Create the blob client and send the delete request
	const BlobClient blob_client =
	    BlobClient::CreateFromConnectionString(GetConnectionStringFromEnv(), names.bucket, names.object);
	try
	{
		const auto delete_res = blob_client.DeleteIfExists();
		if (!delete_res.Value.Deleted)
		{
			spdlog::info("The blob didn't exist.");
		}
		return kSuccess;
	}
	catch (const StorageException& e)
	{
		LogException("Error while deleting blob.", e.what());
		return kFailure;
	}
	catch (const std::exception& e)
	{
		LogException("Error while deleting blob, unrelated to a Storage error.", e.what());
		return kFailure;
	}
}

int driver_rmdir(const char* filename)
{
	ERROR_ON_NULL_ARG(filename, kFailure);

	spdlog::debug("rmdir {}", filename);

	assert(driver_isConnected());
	spdlog::debug("Remove dir (does nothing...)");
	return kSuccess;
}

int driver_mkdir(const char* filename)
{
	ERROR_ON_NULL_ARG(filename, kFailure);

	spdlog::debug("mkdir {}", filename);

	assert(driver_isConnected());
	return kSuccess;
}

long long int driver_diskFreeSpace(const char* filename)
{
	ERROR_ON_NULL_ARG(filename, kFailure);

	spdlog::debug("diskFreeSpace {}", filename);

	assert(driver_isConnected());
	constexpr long long free_space{5LL * 1024LL * 1024LL * 1024LL * 1024LL};
	return free_space;
}

int driver_copyToLocal(const char* sSourceFilePathName, const char* sDestFilePathName)
{
	KH_AZ_CONNECTION_ERROR(kFailure);

	ERROR_ON_NULL_ARG(sSourceFilePathName, kFailure);
	ERROR_ON_NULL_ARG(sDestFilePathName, kFailure);

	spdlog::debug("copyToLocal {} {}", sSourceFilePathName, sDestFilePathName);

	// try opening the online source file
	auto name_parsing_result = ParseAzureUri(sSourceFilePathName);
	ERROR_ON_NAMES(name_parsing_result, kFailure);

	auto& parsed_names = name_parsing_result.GetValue();
	auto reader_ptr_res =
	    MakeReaderPtr(std::move(parsed_names.bucket), std::move(parsed_names.object), ReaderMode::kNone);
	if (!reader_ptr_res)
	{
		LogBadResult(reader_ptr_res, "Error while opening remote file.");
		return kFailure;
	}

	// open local file
	std::ofstream file_stream(sDestFilePathName, std::ios::binary);
	if (!file_stream.is_open())
	{
		std::ostringstream oss;
		oss << "Failed to open local file for writing: " << sDestFilePathName;
		LogError(oss.str());
		return kFailure;
	}

	// limit download to a few MBs at a time.
	constexpr long long dl_limit{10 * 1024 * 1024};
	std::vector<uint8_t> relay_buff(dl_limit);

	Reader& reader = *(reader_ptr_res.GetValue());
	long long& offset = reader.offset_;

	// some constants
	const std::string& bucket = reader.bucketname_;
	const std::vector<std::string>& files = reader.filenames_;
	const size_t nb_files = files.size();
	const std::vector<long long>& cumul_sizes = reader.cumulativeSize_;
	const long long to_read = reader.total_size_;

	// start with
	size_t part = 0;

	while (file_stream && offset < to_read && part < nb_files)
	{
		long long curr_offset =
		    (part == 0) ? offset : offset - cumul_sizes[part - 1] + reader.commonHeaderLength_;
		try
		{
			const auto client =
			    BlobClient::CreateFromConnectionString(GetConnectionStringFromEnv(), bucket, files[part]);
			const size_t read = ReadPart(client, relay_buff, static_cast<int64_t>(curr_offset));
			file_stream.write(reinterpret_cast<const char*>(relay_buff.data()),
					  static_cast<std::streamsize>(read));
			offset += static_cast<long long>(read);
		}
		catch (const StorageException& e)
		{
			LogException("Error while reading from remote file.", e.what());
			offset = 0;
			return kFailure;
		}
		catch (const std::exception& e)
		{
			LogException("Error while copying to local file.", e.what());
			offset = 0;
			return kFailure;
		}

		// is the current part fully read?
		if (offset == cumul_sizes[part])
		{
			part++;
		}
	}
	// did it go wrong?
	if (!file_stream || offset < to_read)
	{
		std::ostringstream oss;
		if (!file_stream)
		{
			oss << "Error while copying data to local file. Writing on local file failed.";
		}
		else
		{
			oss << "Error while copying data to local file. Data is missing.";
		}
		LogError(oss.str());
		offset = 0;
		return kFailure;
	}

	return kSuccess;
}

int driver_copyFromLocal(const char* sSourceFilePathName, const char* sDestFilePathName)
{
	KH_AZ_CONNECTION_ERROR(kFailure);

	if (!sSourceFilePathName || !sDestFilePathName)
	{
		LogError("Error passing null pointers as arguments to copyFromLocal");
		return kFailure;
	}

	spdlog::debug("copyFromLocal {} {}", sSourceFilePathName, sDestFilePathName);

	auto name_parsing_result = ParseAzureUri(sDestFilePathName);
	ERROR_ON_NAMES(name_parsing_result, kFailure);

	// Open the local file
	std::ifstream file_stream(sSourceFilePathName, std::ios::binary);
	if (!file_stream.is_open())
	{
		std::ostringstream oss;
		oss << "Failed to open local file: " << sSourceFilePathName;
		LogError(oss.str());
		return kFailure;
	}
	// size of file
	file_stream.seekg(0, std::ios_base::end);
	const int64_t file_size = static_cast<int64_t>(file_stream.tellg());
	file_stream.seekg(0);

	// Create a writer stream
	auto& parsed_names = name_parsing_result.GetValue();
	auto writer_ptr_res =
	    MakeWriterPtr(std::move(parsed_names.bucket), std::move(parsed_names.object), WriterMode::kWrite);
	if (!writer_ptr_res)
	{
		LogBadResult(writer_ptr_res, "Error while creating writer stream to remote storage.");
		return kFailure;
	}

	// append blobs by chunks of 100 MB, limit allowed by Azure
	const auto& writer = *(writer_ptr_res.GetValue());
	const AppendBlobClient& append_client = writer.client_;

	constexpr int64_t max_size{100 * 1024 * 1024};
	std::vector<uint8_t> relay_buffer(static_cast<size_t>(std::min(max_size, file_size)));

	try
	{
		// TODO: try to avoid copying to the relay buffer by using RandomAccessFileBodyStream. This requires the file descriptor
		// of the opened source file.

		for (int64_t remaining = file_size, to_read = std::min(remaining, max_size);
		     to_read > 0 && file_stream.read(reinterpret_cast<char*>(relay_buffer.data()),
						     static_cast<std::streamsize>(to_read));
		     remaining -= to_read, to_read = std::min(remaining, max_size))
		{
			Azure::Core::IO::MemoryBodyStream memory_stream(relay_buffer.data(),
									static_cast<size_t>(to_read));
			append_client.AppendBlock(memory_stream);
		}
		if (!file_stream)
		{
			// unexpected short read or error of another kind
			LogError("Error while reading from local file.");
			return kFailure;
		}
	}
	catch (const StorageException& e)
	{
		LogException("Error while writing to remote storage due to storage error.", e.what());
		return kFailure;
	}
	catch (const std::exception& e)
	{
		LogException("Error while writing to remote storage unrelated to storage actions.", e.what());
		return kFailure;
	}

	return kSuccess;
}
