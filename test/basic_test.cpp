#include "azureplugin.h"
#include "azureplugin_internal.h"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <iostream>
#include <fstream>  
#include <sstream>  

#include <boost/process/environment.hpp>

#include <boost/uuid/uuid.hpp>            // uuid class
#include <boost/uuid/uuid_generators.hpp> // generators
#include <boost/uuid/uuid_io.hpp>         // streaming operators etc.

#include <gtest/gtest.h>

using namespace azureplugin;
/*
namespace gc = ::google::cloud;
namespace gcs = gc::storage;

using ::testing::Return;
using LOReturnType = gc::StatusOr<gcs::internal::ListObjectsResponse>;
*/

TEST(GCSDriverTest, GetDriverName)
{
    ASSERT_STREQ(driver_getDriverName(), "GCS driver");
}

TEST(GCSDriverTest, GetVersion)
{
    ASSERT_STREQ(driver_getVersion(), "0.1.0");
}

TEST(GCSDriverTest, GetScheme)
{
    ASSERT_STREQ(driver_getScheme(), "gs");
}

TEST(GCSDriverTest, IsReadOnly)
{
    ASSERT_EQ(driver_isReadOnly(), kFalse);
}

TEST(GCSDriverTest, Connect)
{
    //check connection state before call to connect
    ASSERT_EQ(driver_isConnected(), kFalse);

    //call connect and check connection
    ASSERT_EQ(driver_connect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kTrue);

    //call disconnect and check connection
    ASSERT_EQ(driver_disconnect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kFalse);
}

TEST(GCSDriverTest, Disconnect)
{
    ASSERT_EQ(driver_connect(), kSuccess);
    ASSERT_EQ(driver_disconnect(), kSuccess);
    ASSERT_EQ(driver_isConnected(), kFalse);
}

TEST(GCSDriverTest, GetFileSize)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt"), 5585568);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, GetMultipartFileSize)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/Adult-split-00000000000*.txt"), 5585568);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, GetFileSizeNonexistentFailure)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/samples/non_existent_file.txt"), -1);
    ASSERT_STRNE(driver_getlasterror(), NULL);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, FileExists)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_exist("gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt"), kSuccess);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, DirExists)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_exist("gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/"), kSuccess);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

#ifndef _WIN32
// Setting of environment variables does not work on Windows
TEST(GCSDriverTest, DriverConnectMissingCredentialsFailure)
{
    auto env = boost::this_process::environment();
    env["GCP_TOKEN"] = "/tmp/notoken.json";
	ASSERT_EQ(driver_connect(), kFailure);
    env.erase("GCP_TOKEN");
}

void setup_bad_credentials() {
    std::stringstream tempCredsFile;
#ifdef _WIN32
	tempCredsFile << std::getenv("TEMP") << "\\creds-" << boost::uuids::random_generator()() << ".json";
#else
	tempCredsFile << "/tmp/creds-" << boost::uuids::random_generator()() << ".json";
#endif
    std::ofstream outfile (tempCredsFile.str());
    outfile << "{}" << std::endl;
    outfile.close();
    auto env = boost::this_process::environment();
    env["GCP_TOKEN"] = tempCredsFile.str();
}

void cleanup_bad_credentials() {
    auto env = boost::this_process::environment();
    env.erase("GCP_TOKEN");
}

TEST(GCSDriverTest, GetFileSizeInvalidCredentialsFailure)
{
    setup_bad_credentials();
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_getFileSize("gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt"), -1);
    ASSERT_STRNE(driver_getlasterror(), NULL);
	ASSERT_EQ(driver_disconnect(), kSuccess);
    cleanup_bad_credentials();
}
#endif

TEST(GCSDriverTest, RmDir)
{
    ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_rmdir("dummy"), kSuccess);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, mkDir)
{
	ASSERT_EQ(driver_connect(), kSuccess);
	ASSERT_EQ(driver_mkdir("dummy"), kSuccess);
	ASSERT_EQ(driver_disconnect(), kSuccess);
}

TEST(GCSDriverTest, GetSystemPreferredBufferSize)
{
	ASSERT_EQ(driver_getSystemPreferredBufferSize(), 4 * 1024 * 1024);
}

constexpr const char* test_dir_name = "gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/";

constexpr const char* test_single_file = "gs://data-test-khiops-driver-gcs/khiops_data/samples/Adult/Adult.txt";
constexpr const char* test_range_file_one_header = "gs://data-test-khiops-driver-gcs/khiops_data/split/Adult/Adult-split-0[0-5].txt";
constexpr const char* test_glob_file_header_each = "gs://data-test-khiops-driver-gcs/khiops_data/bq_export/Adult/*.txt";
constexpr const char* test_double_glob_header_each = "gs://data-test-khiops-driver-gcs/khiops_data/split/Adult_subsplit/**/Adult-split-*.txt";

constexpr std::array<const char*, 4> test_files = {
    test_single_file,
    test_range_file_one_header,
    test_glob_file_header_each,
    test_double_glob_header_each
};
