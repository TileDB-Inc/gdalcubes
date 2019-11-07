

#include "vector_queries.h"
#include <ogrsf_frmts.h>
#include <thread>
#include <gdal_utils.h>

namespace gdalcubes {

std::vector<std::vector<double>> vector_queries::query_points(std::shared_ptr<cube> cube, std::vector<double> x,
                                                              std::vector<double> y, std::vector<std::string> t,
                                                              std::string srs) {
    // make sure that x, y, and t have same size
    if (x.size() != y.size() || y.size() != t.size()) {
        GCBS_ERROR("Point coordinate vectors x, y, t must have identical length");
        throw std::string("Point coordinate vectors x, y, t must have identical length");
    }

    if (x.empty()) {
        GCBS_ERROR("Point coordinate vectors x, y, t must have length > 0");
        throw std::string("Point coordinate vectors x, y, t must have length > 0");
    }

    if (!cube) {
        GCBS_ERROR("Invalid data cube pointer");
        throw std::string("Invalid data cube pointer");
    }

    uint32_t nthreads = config::instance()->get_default_chunk_processor()->max_threads();

    std::shared_ptr<progress> prg = config::instance()->get_default_progress_bar()->get();
    prg->set(0);  // explicitly set to zero to show progress bar immediately

    // coordinate transformation
    if (cube->st_reference()->srs() != srs) {
        OGRSpatialReference srs_in;
        OGRSpatialReference srs_out;
        srs_in.SetFromUserInput(cube->st_reference()->srs().c_str());
        srs_out.SetFromUserInput(srs.c_str());

        if (!srs_in.IsSame(&srs_out)) {
            std::vector<std::thread> workers_transform;
            uint32_t n = (uint32_t)std::ceil(double(x.size()) / double(nthreads));  // points per thread

            for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
                workers_transform.push_back(std::thread([&cube, &srs, &srs_in, &srs_out, &x, &y, ithread, n](void) {
                    OGRCoordinateTransformation *coord_transform = OGRCreateCoordinateTransformation(&srs_in, &srs_out);

                    int begin = ithread * n;
                    int end = std::min(uint32_t(ithread * n + n), uint32_t(x.size()));
                    int count = end - begin;

                    // change coordinates in place, should be safe because vectors don't change their sizes
                    if (count > 0) {
                        if (coord_transform == nullptr || !coord_transform->Transform(count, x.data() + begin, y.data() + begin)) {
                            throw std::string("ERROR: coordinate transformation failed (from " + cube->st_reference()->srs() + " to " + srs + ").");
                        }
                    }
                    OCTDestroyCoordinateTransformation(coord_transform);
                }));
            }
            for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
                workers_transform[ithread].join();
            }
        }
    }

    // TODO: possible without additional copy?
    std::vector<double> it;  // array indexes
                             //    ix.resize(x.size());
                             //    iy.resize(x.size());
    it.resize(x.size());

    std::map<chunkid_t, std::vector<uint32_t>> chunk_index;

    std::vector<std::thread> workers_preprocess;
    std::mutex mtx;
    for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
        workers_preprocess.push_back(std::thread([&mtx, &cube, &x, &y, &t, &it, &chunk_index, ithread, nthreads](void) {
            for (uint32_t i = ithread; i < x.size(); i += nthreads) {
                coords_st st;

                st.s.x = x[i];
                st.s.y = y[i];

                // array coordinates
                x[i] = (x[i] - cube->st_reference()->left()) / cube->st_reference()->dx();
                //iy.push_back(cube->st_reference()->ny() - 1 - ((y[i] - cube->st_reference()->bottom()) / cube->st_reference()->dy()));  // top 0
                y[i] = (y[i] - cube->st_reference()->bottom()) / cube->st_reference()->dy();

                datetime dt = datetime::from_string(t[i]);
                if (dt.unit() > cube->st_reference()->dt().dt_unit) {
                    dt.unit() = cube->st_reference()->dt().dt_unit;
                    GCBS_WARN("date / time of query point has coarser granularity than the data cube; converting '" + t[i] + "' -> '" + dt.to_string() + "'");
                } else {
                    dt.unit() = cube->st_reference()->dt().dt_unit;
                }
                duration delta = cube->st_reference()->dt();
                it[i] = (dt - cube->st_reference()->t0()) / delta;

                if (it[i] < 0 || it[i] >= cube->size_t() ||
                    x[i] < 0 || x[i] >= cube->size_x() ||
                    y[i] < 0 || y[i] >= cube->size_y()) {  // if point is outside of the cube
                    continue;
                }
                st.t = dt;
                chunkid_t c = cube->find_chunk_that_contains(st);

                mtx.lock();
                chunk_index[c].push_back(i);
                mtx.unlock();
            }
        }));
    }
    for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
        workers_preprocess[ithread].join();
    }

    std::vector<std::vector<double>> out;
    out.resize(cube->bands().count());
    for (uint16_t ib = 0; ib < out.size(); ++ib) {
        out[ib].resize(x.size(), NAN);
    }

    std::vector<chunkid_t> chunks;  // vector of keys in chunk_index
    for (auto iter = chunk_index.begin(); iter != chunk_index.end(); ++iter) {
        chunks.push_back(iter->first);
    }

    std::vector<std::thread> workers;
    for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
        workers.push_back(std::thread([&prg, &cube, &out, &chunk_index, &chunks, &x, &it, &y, ithread, nthreads](void) {
            for (uint32_t ic = ithread; ic < chunks.size(); ic += nthreads) {
                try {
                    if (chunks[ic] < cube->count_chunks()) {  // if chunk exists
                        std::shared_ptr<chunk_data> dat = cube->read_chunk(chunks[ic]);
                        if (!dat->empty()) {  // if chunk is not empty
                            // iterate over all query points within the current chunk
                            for (uint32_t i = 0; i < chunk_index[chunks[ic]].size(); ++i) {
                                double ixc = x[chunk_index[chunks[ic]][i]];
                                double iyc = y[chunk_index[chunks[ic]][i]];
                                double itc = it[chunk_index[chunks[ic]][i]];

                                int iix = ((int)std::floor(ixc)) % cube->chunk_size()[2];
                                int iiy = dat->size()[2] - 1 - (((int)std::floor(iyc)) % cube->chunk_size()[1]);
                                int iit = ((int)std::floor(itc)) % cube->chunk_size()[0];

                                // check to prevent out of bounds faults
                                if (iix < 0 || uint32_t(iix) >= dat->size()[3]) continue;
                                if (iiy < 0 || uint32_t(iiy) >= dat->size()[2]) continue;
                                if (iit < 0 || uint32_t(iit) >= dat->size()[1]) continue;

                                for (uint16_t ib = 0; ib < out.size(); ++ib) {
                                    out[ib][chunk_index[chunks[ic]][i]] = ((double *)dat->buf())[ib * dat->size()[1] * dat->size()[2] * dat->size()[3] + iit * dat->size()[2] * dat->size()[3] + iiy * dat->size()[3] + iix];
                                }
                            }
                        }
                    }
                    prg->increment((double)1 / (double)chunks.size());
                } catch (std::string s) {
                    GCBS_ERROR(s);
                    continue;
                } catch (...) {
                    GCBS_ERROR("unexpected exception while processing chunk " + std::to_string(chunks[ic]));
                    continue;
                }
            }
        }));
    }
    for (uint32_t ithread = 0; ithread < nthreads; ++ithread) {
        workers[ithread].join();
    }
    prg->finalize();

    return out;
}

struct zonal_statistics_func {
    zonal_statistics_func() : _nfeatures(0), _nt(0){};
    virtual ~zonal_statistics_func(){};

    virtual void init(uint32_t nfeatures, uint32_t nt) {
        _nfeatures = nfeatures;
        _nt = nt;
    };
    virtual void update(double x, uint32_t ifeature, uint32_t it) = 0;
    virtual std::shared_ptr<std::vector<double>> finalize() = 0;

   protected:
    uint32_t _nfeatures;
    uint32_t _nt;
};

struct zonal_statistics_count : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, 0);
    }

    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) (*_x)[ifeature * _nt + it] += 1;
    }

    std::shared_ptr<std::vector<double>> finalize() override {
        return _x;
    }

    std::shared_ptr<std::vector<double>> _x;
};

struct zonal_statistics_sum : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, NAN);
    }

    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            if (std::isnan((*_x)[ifeature * _nt + it])) {
                (*_x)[ifeature * _nt + it] = x;
            } else {
                (*_x)[ifeature * _nt + it] += x;
            }
        }
    }

    std::shared_ptr<std::vector<double>> finalize() override {
        return _x;
    }

    std::shared_ptr<std::vector<double>> _x;
};

struct zonal_statistics_prod : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, NAN);
    }

    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            if (std::isnan((*_x)[ifeature * _nt + it])) {
                (*_x)[ifeature * _nt + it] = x;
            } else {
                (*_x)[ifeature * _nt + it] *= x;
            }
        }
    }

    std::shared_ptr<std::vector<double>> finalize() override {
        return _x;
    }

    std::shared_ptr<std::vector<double>> _x;
};

struct zonal_statistics_mean : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, 0);
        _n.resize(_nt * _nfeatures, 0);
    }

    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            (*_x)[ifeature * _nt + it] += x;
            _n[ifeature * _nt + it]++;
        }
    }

    std::shared_ptr<std::vector<double>> finalize() override {
        for (uint32_t i = 0; i < _nfeatures * _nt; ++i) {
            (*_x)[i] = (*_x)[i] / double(_n[i]);
        }
        return _x;
    }

    std::shared_ptr<std::vector<double>> _x;
    std::vector<uint32_t> _n;
};

struct zonal_statistics_min : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, std::numeric_limits<double>::max());
    }
    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            (*_x)[ifeature * _nt + it] = std::min(x, (*_x)[ifeature * _nt + it]);
        }
    }
    std::shared_ptr<std::vector<double>> finalize() override {
        for (uint32_t i = 0; i < _nfeatures * _nt; ++i) {
            if ((*_x)[i] == std::numeric_limits<double>::max()) {
                (*_x)[i] = NAN;
            }
        }
        return _x;
    }
    std::shared_ptr<std::vector<double>> _x;
};

struct zonal_statistics_max : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _x = std::make_shared<std::vector<double>>();
        _x->resize(_nt * _nfeatures, std::numeric_limits<double>::lowest());
    }
    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            (*_x)[ifeature * _nt + it] = std::max(x, (*_x)[ifeature * _nt + it]);
        }
    }
    std::shared_ptr<std::vector<double>> finalize() override {
        for (uint32_t i = 0; i < _nfeatures * _nt; ++i) {
            if ((*_x)[i] == std::numeric_limits<double>::lowest()) {
                (*_x)[i] = NAN;
            }
        }
        return _x;
    }
    std::shared_ptr<std::vector<double>> _x;
};

struct zonal_statistics_median : public zonal_statistics_func {
    void init(uint32_t nfeatures, uint32_t nt) override {
        zonal_statistics_func::init(nfeatures, nt);
        _values.resize(_nt * _nfeatures);
    }

    void update(double x, uint32_t ifeature, uint32_t it) override {
        if (std::isfinite(x)) {
            _values[ifeature * _nt + it].push_back(x);
        }
    }

    std::shared_ptr<std::vector<double>> finalize() override {
        std::shared_ptr<std::vector<double>> out = std::make_shared<std::vector<double>>();
        out->resize(_nt * _nfeatures);
        for (uint32_t i = 0; i < _nfeatures * _nt; ++i) {
            std::sort(_values[i].begin(), _values[i].end());
            if (_values[i].size() == 0) {
                (*out)[i] = NAN;
            } else if (_values[i].size() % 2 == 1) {
                (*out)[i] = _values[i][_values[i].size() / 2];
            } else {
                (*out)[i] = (_values[i][_values[i].size() / 2] + _values[i][_values[i].size() / 2 - 1]) / ((double)2);
            }
        }
        return out;
    }

    std::vector<std::vector<double>> _values;
};

// TODO: implement var and sd aggregation functions
    std::vector<std::string> vector_queries::zonal_statistics(std::shared_ptr<cube> cube, std::string ogr_dataset,
                                      std::vector<std::pair<std::string, std::string>> agg_band_functions,
                                      std::string out_dir, std::string out_prefix, std::string ogr_layer) {
    if (!OGRGeometryFactory::haveGEOS()) {
        GCBS_ERROR("Missing GEOS support in GDAL installation");
        throw std::string("Missing GEOS support in GDAL installation");
    }

    std::vector<uint16_t> band_index;
    std::vector<std::string> agg_func_names;
    std::vector<std::function<std::unique_ptr<zonal_statistics_func>()>> agg_func_creators;
    for (uint16_t i = 0; i < agg_band_functions.size(); ++i) {
        if (!cube->bands().has(agg_band_functions[i].second)) {
            GCBS_WARN("Data cube has no band '" + agg_band_functions[i].second + "', statistics on this band will be ignored");
        } else {
            if (agg_band_functions[i].first == "min") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_min()); });
            } else if (agg_band_functions[i].first == "max") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_max()); });
            } else if (agg_band_functions[i].first == "count") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_count()); });
            } else if (agg_band_functions[i].first == "sum") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_sum()); });
            } else if (agg_band_functions[i].first == "prod") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_prod()); });
            } else if (agg_band_functions[i].first == "mean") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_mean()); });
            } else if (agg_band_functions[i].first == "median") {
                agg_func_creators.push_back([]() { return std::unique_ptr<zonal_statistics_func>(new zonal_statistics_median()); });
            }  // TODO: Add sd and var
            else {
                GCBS_WARN("There is no aggregation function '" + agg_band_functions[i].first + "', related summary statistics will be ignored.");
                continue;
            }
            band_index.push_back(cube->bands().get_index(agg_band_functions[i].second));
            agg_func_names.push_back(agg_band_functions[i].first);
        }
    }

    // open input OGR dataset
    GDALDataset *in_ogr_dataset;

    in_ogr_dataset = (GDALDataset *)GDALOpenEx(ogr_dataset.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL,
                                               NULL);
    if (in_ogr_dataset == NULL) {
        GCBS_ERROR("failed to open '" + ogr_dataset + "'");
        throw std::string("failed to open '" + ogr_dataset + "'");
    }

    OGRLayer *layer;
    if (in_ogr_dataset->GetLayerCount() > 1) {
        if (ogr_layer.empty()) {
            GCBS_WARN("input OGR dataset has multiple layers, using the first.");
            layer = in_ogr_dataset->GetLayer(0);
        } else {
            layer = in_ogr_dataset->GetLayerByName(ogr_layer.c_str());
        }
    } else {
        if (ogr_layer.empty()) {
            layer = in_ogr_dataset->GetLayer(0);
        } else {
            layer = in_ogr_dataset->GetLayerByName(ogr_layer.c_str());
        }
    }
    if (layer == NULL) {
        GCBS_ERROR("invalid OGR layer");
        throw std::string("invalid OGR layer");
    }

    // If layer has more than one geometry column, only the first will be used.
    // Warn if there are more geometry columns
    if (layer->GetLayerDefn()->GetGeomFieldCount() > 1) {
        std::string geom_field_name = layer->GetLayerDefn()->GetGeomFieldDefn(0)->GetNameRef();
        GCBS_WARN("Found more than one geometry field for input features, using only the first ('" + geom_field_name + "'");
    }

    // Make sure that geometry column has type Polygon / MultiPolygon
    OGRwkbGeometryType geom_type = layer->GetGeomType();
    if (geom_type != wkbPolygon && geom_type != wkbPolygonM && geom_type != wkbPolygon25D &&
        geom_type != wkbPolygonZM && geom_type != wkbMultiPolygon) {
        GCBS_ERROR("Zonal statistics requires Polygon or MultiPolygon input geometries");
        GDALClose(in_ogr_dataset);
        // TODO: do we have to clean up more things here?
        throw std::string("Zonal statistics requires Polygon or MultiPolygon input geometries");
    }

    // check that cube and ogr dataset have same spatial reference system.
    OGRSpatialReference srs_cube = cube->st_reference()->srs_ogr();
    OGRSpatialReference *srs_features = layer->GetSpatialRef();

    if (!srs_cube.IsSame(srs_features)) {
        GCBS_ERROR("Data cube and input features have different SRSes");
        GDALClose(in_ogr_dataset);
        // TODO: do we have to clean up more things here?
        throw std::string("Data cube and input features have different SRSes");
    }

    // Helper data structure: map: chunk index -> vector of FIDs of all features intersecting the chunk
    std::map<chunkid_t, std::vector<int32_t>> features_in_chunk;

    // Check assumption: Input dataset has FIDs
    std::string fid_column = layer->GetFIDColumn();
    if (fid_column.empty()) {
        GCBS_ERROR("Input feature dataset must have FIDs");
        throw std::string("Input feature dataset must have FIDs");
    }

    if (!layer->TestCapability(OLCRandomRead)) {
        GCBS_WARN("Input feature layer does not support efficient random reads; computations may take a considerable amount of time.");
    }

    // start progress bar
    std::shared_ptr<progress> prg = config::instance()->get_default_progress_bar()->get();
    prg->set(0);  // explicitly set to zero to show progress bar immediately

    // TODO: use SetSpatialFilterRect() ???

    for (uint32_t cx = 0; cx < cube->count_chunks_x(); ++cx) {
        for (uint32_t cy = 0; cy < cube->count_chunks_y(); ++cy) {
            chunkid_t id = cube->chunk_id_from_coords({0, cy, cx});
            bounds_2d<double> sextent = cube->bounds_from_chunk(id).s;
            OGRPolygon pp;
            OGRLinearRing a;

            a.addPoint(sextent.left, sextent.bottom);
            a.addPoint(sextent.right, sextent.bottom);
            a.addPoint(sextent.right, sextent.top);
            a.addPoint(sextent.left, sextent.top);
            a.addPoint(sextent.left, sextent.bottom);
            pp.addRing(&a);
            pp.assignSpatialReference(&srs_cube);

            // iterate over all features
            layer->ResetReading();
            OGRFeature *cur_feature;
            while ((cur_feature = layer->GetNextFeature()) != NULL) {
                OGRGeometry *cur_geometry = cur_feature->GetGeometryRef();
                if (cur_geometry != NULL) {
                    if (cur_geometry->Intersects(&pp)) {
                        features_in_chunk[id].push_back(cur_feature->GetFID());
                    }
                }
                OGRFeature::DestroyFeature(cur_feature);
            }

        }
    }

    // Helper data structure: map: FID -> internal integer index
    std::unordered_map<int32_t, int32_t> FID_of_index;
    std::unordered_map<int32_t, int32_t> index_of_FID;

    uint32_t nfeatures = layer->GetFeatureCount();

    layer->ResetReading();
    OGRFeature *cur_feature;
    int32_t cur_index = 0;
    while ((cur_feature = layer->GetNextFeature()) != NULL) {
        FID_of_index[cur_index] = cur_feature->GetFID();
        index_of_FID[cur_feature->GetFID()] = cur_index;
        cur_index++;
        OGRFeature::DestroyFeature(cur_feature);
    }
    layer->ResetReading();

    std::vector<std::string> out_datasets;

    for (uint32_t ct = 0; ct < cube->count_chunks_t(); ++ct) {
        // TODO: define body of the loop as lambda function and evaluate multithreaded


        // initialize per geometry + time aggregators
        uint32_t nt = cube->chunk_size(cube->chunk_id_from_coords({ct, 0, 0}))[0];
        std::vector<std::unique_ptr<zonal_statistics_func>> pixel_aggregators;
        for (uint16_t i = 0; i < agg_func_names.size(); ++i) {
            pixel_aggregators.push_back(agg_func_creators[i]());
            pixel_aggregators[i]->init(nfeatures, nt);
        }

        for (uint32_t cx = 0; cx < cube->count_chunks_x(); ++cx) {
            for (uint32_t cy = 0; cy < cube->count_chunks_y(); ++cy) {
                chunkid_t id = cube->chunk_id_from_coords({ct, cy, cx});
                chunkid_t id_spatial = cube->chunk_id_from_coords({0, cy, cx});

                bounds_st chunk_bounds = cube->bounds_from_chunk(id);

                // if chunk intersects with at least one geometry
                if (features_in_chunk.count(id_spatial) > 0) {
                    // read chunk
                    std::shared_ptr<chunk_data> chunk = cube->read_chunk(id);

                    if (chunk->empty()) {
                        continue;
                    }

                    // iterate over all features that intersect spatially with current chunk
                    for (uint32_t ifeature = 0; ifeature < features_in_chunk[id_spatial].size(); ++ifeature) {
                        int32_t fid = features_in_chunk[id_spatial][ifeature];

                        // filter chunk pixels by bounding box of current feature
                        OGRFeature *feature = layer->GetFeature(fid);
                        OGREnvelope feature_bbox;
                        feature->GetGeometryRef()->getEnvelope(&feature_bbox);

                        int32_t x_start = std::max((int32_t)std::floor((feature_bbox.MinX - chunk_bounds.s.left) / cube->st_reference()->dx()), 0);
                        int32_t x_end = std::min((int32_t)std::ceil((feature_bbox.MaxX - chunk_bounds.s.left) / cube->st_reference()->dx()), (int32_t)chunk->size()[3] - 1);

                        bool outside = false;
                        if (x_end < 0 || x_start > int32_t(chunk->size()[3] - 1)) {
                            outside = true;
                        }

                        int32_t y_start = std::max((int32_t)std::floor((chunk_bounds.s.top - feature_bbox.MaxY) / cube->st_reference()->dy()), 0);
                        int32_t y_end = std::min((int32_t)std::ceil((chunk_bounds.s.top - feature_bbox.MinY) / cube->st_reference()->dy()), (int32_t)chunk->size()[2] - 1);

                        if (y_end < 0 || y_start > int32_t(chunk->size()[2] - 1)) {
                            outside = true;
                        }

                        if (!outside) {

                            // rasterize
                            int err = 0;
                            CPLStringList rasterize_args;
                            rasterize_args.AddString("-burn");
                            rasterize_args.AddString("1");
                            rasterize_args.AddString("-ot");
                            rasterize_args.AddString("Byte");
                            rasterize_args.AddString("-of");
                            rasterize_args.AddString("MEM");
                            rasterize_args.AddString("-init");
                            rasterize_args.AddString("0");
                            rasterize_args.AddString("-tr");
                            rasterize_args.AddString(std::to_string(cube->st_reference()->dx()).c_str());
                            rasterize_args.AddString(std::to_string(cube->st_reference()->dy()).c_str());
                            rasterize_args.AddString("-te");
                            rasterize_args.AddString(std::to_string(chunk_bounds.s.left + x_start * cube->st_reference()->dx()).c_str()); // xmin
                            rasterize_args.AddString(std::to_string(chunk_bounds.s.top - (y_end+1) * cube->st_reference()->dy()).c_str()); // ymin
                            rasterize_args.AddString(std::to_string(chunk_bounds.s.left + (x_end + 1) * cube->st_reference()->dx()).c_str()); // xmax
                            rasterize_args.AddString(std::to_string(chunk_bounds.s.top - y_start * cube->st_reference()->dy()).c_str()); // ymax
                            rasterize_args.AddString("-where");
                            std::string where =  fid_column + "=" + std::to_string(fid);
                            rasterize_args.AddString(where.c_str());
                            rasterize_args.AddString("-l");
                            rasterize_args.AddString(layer->GetName());

                            // log gdal_rasterize call
//                            std::stringstream ss;
//                            ss << "Running gdal_rasterize ";
//                            for (uint16_t iws = 0; iws < rasterize_args.size(); ++iws) {
//                                ss << rasterize_args[iws] << " ";
//                            }
//                            ss << ogr_dataset;
//                            GCBS_TRACE(ss.str());

                            GDALRasterizeOptions* rasterize_opts =  GDALRasterizeOptionsNew(rasterize_args.List(), NULL);
                            if (rasterize_opts == NULL) {
                                GDALRasterizeOptionsFree(rasterize_opts);
                                throw std::string("ERROR in vector_queries::zonal_statistics(): cannot create gdal_rasterize options.");
                            }

                            GDALDataset *gdal_rasterized = (GDALDataset *)GDALRasterize("", NULL, (GDALDatasetH)in_ogr_dataset, rasterize_opts, &err );
                            if (gdal_rasterized == NULL) {
                                GCBS_ERROR("gdal_rasterize failed for feature with FID " + std::to_string(fid));
                            }
                            GDALRasterizeOptionsFree(rasterize_opts);


                            uint8_t* geom_mask = (uint8_t*)std::malloc(sizeof(uint8_t) * (x_end - x_start + 1) * (y_end - y_start + 1));
                            if (gdal_rasterized->GetRasterBand(1)->RasterIO(GF_Read,0, 0, x_end - x_start + 1, y_end - y_start + 1, geom_mask, x_end - x_start + 1, y_end - y_start + 1, GDT_Byte, 0, 0, NULL) != CE_None) {
                                GCBS_ERROR("RasterIO failed for feature with FID " + std::to_string(fid));
                            }


                            for (int32_t iy = y_start; iy <= y_end; ++iy) {
                                for (int32_t ix = x_start; ix <= x_end; ++ix) {

                                    // if mask is 1
                                    if (geom_mask[(iy-y_start) * (x_end - x_start + 1) + ix - x_start] == 1)
                                    {
                                        for (uint32_t it = 0; it < chunk->size()[1]; ++it) {
                                            for (uint16_t ifield = 0; ifield < agg_func_names.size(); ++ifield) {
                                                uint16_t b_index = band_index[ifield];
                                                double v = ((double *)chunk->buf())[b_index * chunk->size()[1] * chunk->size()[2] * chunk->size()[3] +
                                                                                    it * chunk->size()[2] * chunk->size()[3] +
                                                                                    iy * chunk->size()[3] +
                                                                                    ix];
                                                pixel_aggregators[ifield]->update(v, index_of_FID[fid], it);
                                            }
                                        }
                                    }
                                }
                            }
                            std::free(geom_mask);
                            GDALClose(gdal_rasterized);
                        }
                        OGRFeature::DestroyFeature(feature);
                    }
                }
            }
        }

        std::vector<std::shared_ptr<std::vector<double>>> res;
        for (uint16_t iband = 0; iband < agg_func_names.size(); ++iband) {
            res.push_back(pixel_aggregators[iband]->finalize());
        }

        // write output OGR datasets
        for (uint32_t it = 0; it < nt; ++it) {
            GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName("GPKG");
            if (poDriver == NULL) {
                GCBS_ERROR("OGR GeoPackage driver not found");
                throw std::string("OGR GeoPackage driver not found");
            }

            std::string output_file = filesystem::join(out_dir, out_prefix + (cube->st_reference()->t0() + cube->st_reference()->dt() * (it + cube->chunk_limits({ct, 0, 0}).low[0])).to_string() + ".gpkg");
            // TODO: check creation options etc.
            GDALDataset *poDS = poDriver->Create(output_file.c_str(), 0, 0, 0, GDT_Unknown, NULL);
            if (poDS == NULL) {
                GCBS_ERROR("Creation of GPKG file '" + output_file + "' failed");
                throw std::string("Creation of GPKG file '" + output_file + "' failed");
            }

            // TODO: change output layer name

            // TODO: what if layer argument is set
            OGRLayer *poLayer = poDS->CreateLayer(out_prefix.c_str(), srs_features, geom_type, NULL);
            if (poLayer == NULL) {
                GCBS_ERROR("Failed to create output layer in  '" + output_file + "'");
                throw std::string("Failed to create output layer in  '" + output_file + "'");
            }

            for (uint16_t ifield = 0; ifield < agg_func_names.size(); ++ifield) {
                std::string field_name = cube->bands().get(band_index[ifield]).name + "_" + agg_func_names[ifield];
                OGRFieldDefn oField(field_name.c_str(), OFTReal);
                // TODO: set precision? set_nullable?

                if (poLayer->CreateField(&oField) != OGRERR_NONE) {
                    GCBS_ERROR("Failed to create output field '" + field_name + "' in  '" + output_file + "'");
                    throw std::string("Failed to create output field '" + field_name + "' in  '" + output_file + "'");
                }
            }

            for (uint32_t ifeature = 0; ifeature < nfeatures; ++ifeature) {
                OGRFeature *poFeature = OGRFeature::CreateFeature(poLayer->GetLayerDefn());
                for (uint16_t ifield = 0; ifield < agg_func_names.size(); ++ifield) {
                    poFeature->SetField(ifield, (*(res[ifield]))[ifeature * nt + it]);
                }
                OGRFeature *in_feature = layer->GetFeature(FID_of_index[ifeature]);
                // TODO: if in_feature != NULL?
                poFeature->SetGeometry(in_feature->GetGeometryRef());
                if (poLayer->CreateFeature(poFeature) != OGRERR_NONE) {
                    GCBS_ERROR("Failed to create output feature with FID '" + std::to_string(FID_of_index[ifeature]) + "' in  '" + output_file + "'");
                    throw std::string("Failed to create output feature with FID '" + std::to_string(FID_of_index[ifeature]) + "' in  '" + output_file + "'");
                }
                OGRFeature::DestroyFeature(poFeature);
                OGRFeature::DestroyFeature(in_feature);
            }
            GDALClose(poDS);
            out_datasets.push_back(output_file);
            prg->increment(double(1) / cube->size_t());


        }
    }

    GDALClose(in_ogr_dataset);
    prg->finalize();

    return out_datasets;
}

}  // namespace gdalcubes