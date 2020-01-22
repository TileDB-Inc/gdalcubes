/*
    MIT License

    Copyright (c) 2019 Marius Appel <marius.appel@uni-muenster.de>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "config.h"
#include "cube.h"

#include <curl/curl.h>
#include <gdal_priv.h>

namespace gdalcubes {

config* config::_instance = nullptr;

config::config() : _chunk_processor(std::make_shared<chunk_processor_singlethread>()),
                   _progress_bar(std::make_shared<progress_none>()),
                   _error_handler(error_handler::default_error_handler),
                   _gdal_cache_max(1024 * 1024 * 256),         // 256 MiB
                   _server_chunkcache_max(1024 * 1024 * 512),  // 512 MiB
                   _server_worker_threads_max(1),
                   _swarm_curl_verbose(false),
                   _gdal_num_threads(1),
                   _streaming_dir(filesystem::get_tempdir()),
                   _collection_format_preset_dirs() {}

version_info config::get_version_info() {
    version_info v;
    v.VERSION_MAJOR = GDALCUBES_VERSION_MAJOR;
    v.VERSION_MINOR = GDALCUBES_VERSION_MINOR;
    v.VERSION_PATCH = GDALCUBES_VERSION_PATCH;
    v.BUILD_DATE = __DATE__;
    v.BUILD_TIME = __TIME__;
    v.GIT_COMMIT = GDALCUBES_GIT_COMMIT;
    v.GIT_DESC = GDALCUBES_GIT_DESC;
    return v;
}

void config::set_gdal_cache_max(uint32_t size_bytes) {
    GDALSetCacheMax(size_bytes);
    _gdal_cache_max = size_bytes;
}

void config::set_gdal_debug(bool debug) {
    _gdal_debug = debug;
    if (debug)
        CPLSetConfigOption("CPL_DEBUG", "ON");
    else
        CPLSetConfigOption("CPL_DEBUG", "OFF");
}

void config::set_gdal_num_threads(uint16_t threads) {
    _gdal_num_threads = threads;
    CPLSetConfigOption("GDAL_NUM_THREADS", std::to_string(_gdal_num_threads).c_str());
}

void config::gdalcubes_init() {
    curl_global_init(CURL_GLOBAL_ALL);
    GDALAllRegister();
    GDALSetCacheMax(_gdal_cache_max);
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");  // avoid aux files for PNG tiles
    curl_global_init(CURL_GLOBAL_ALL);
    CPLSetConfigOption("GDAL_NUM_THREADS", std::to_string(_gdal_num_threads).c_str());
    //srand(time(NULL)); // R will complain if calling srand...
    CPLSetErrorHandler(CPLQuietErrorHandler);

    // For GDAL 3, force traditional x, y (lon, lat) order
#if GDAL_VERSION_MAJOR > 2
    CPLSetConfigOption("OGR_CT_FORCE_TRADITIONAL_GIS_ORDER", "YES");
#endif

    // Add default locations where to look for collection format presets

    if (std::getenv("GDALCUBES_DATA_DIR") != NULL) {
        if (filesystem::exists(std::getenv("GDALCUBES_DATA_DIR"))) {
            config::instance()->add_collection_format_preset_dir(std::getenv("GDALCUBES_DATA_DIR"));
        }
    }
    if (std::getenv("AllUsersProfile") != NULL) {
        // Windows default location
        std::string p = filesystem::join(filesystem::join(std::getenv("AllUsersProfile"), "gdalcubes"), "formats");
        if (filesystem::exists(p)) {
            config::instance()->add_collection_format_preset_dir(p);
        }
    }
    if (std::getenv("HOME") != NULL) {
        std::string p = filesystem::join(filesystem::join(std::getenv("HOME"), ".gdalcubes"), "formats");
        if (filesystem::exists(p)) {
            config::instance()->add_collection_format_preset_dir(p);
        }
    }
    if (std::getenv("HOMEPATH") != NULL && getenv("HOMEDRIVE") != NULL) {
        std::string p = filesystem::join(filesystem::join(std::getenv("HOMEPATH"), ".gdalcubes"), "formats");
        if (filesystem::exists(p)) {
            config::instance()->add_collection_format_preset_dir(p);
        }
    }

    const std::vector<std::string> candidate_dirs =
        {"/usr/lib/gdalcubes/formats"};

    for (uint16_t i = 0; i < candidate_dirs.size(); ++i) {
        if (filesystem::exists(candidate_dirs[i])) {
            config::instance()->add_collection_format_preset_dir(candidate_dirs[i]);
        }
    }
}

void config::gdalcubes_cleanup() {
    curl_global_cleanup();
    GDALDestroyDriverManager();
    OGRCleanupAll();
}

void config::add_collection_format_preset_dir(std::string dir) {
    // only add if not exists
    for (uint16_t i = 0; i < _collection_format_preset_dirs.size(); ++i) {
        if (_collection_format_preset_dirs[i] == dir) return;
    }
    _collection_format_preset_dirs.push_back(dir);
}

std::string config::gdal_version_info() {
    return GDALVersionInfo("--version");
}

std::vector<std::string> config::gdal_formats() {
    std::vector<std::string> out;
    for (int i = 0; i < GDALGetDriverCount(); ++i) {
        out.push_back(GDALGetDriverShortName(GDALGetDriver(i)));
    }
    return out;
}

}  // namespace gdalcubes
