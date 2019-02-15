/*
   Copyright 2018 Marius Appel <marius.appel@uni-muenster.de>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef REDUCE_SPACE_H
#define REDUCE_SPACE_H

#include "cube.h"

/**
 * @brief A data cube that applies reducer functions over selected bands of a data cube over space
 */
class reduce_space_cube : public cube {
   public:
    /**
     * @brief Create a data cube that applies a reducer function on a given input data cube over space
     * @note This static creation method should preferably be used instead of the constructors as
     * the constructors will not set connections between cubes properly.
     * @param in input data cube
     * @param reducer reducer function
     * @return a shared pointer to the created data cube instance
     */
    static std::shared_ptr<reduce_space_cube> create(std::shared_ptr<cube> in, std::vector<std::pair<std::string, std::string>> reducer_bands) {
        std::shared_ptr<reduce_space_cube> out = std::make_shared<reduce_space_cube>(in, reducer_bands);
        in->add_child_cube(out);
        out->add_parent_cube(in);
        return out;
    }

   public:
    reduce_space_cube(std::shared_ptr<cube> in, std::vector<std::pair<std::string, std::string>> reducer_bands) : cube(std::make_shared<cube_st_reference>(*(in->st_reference()))), _in_cube(in), _reducer_bands(reducer_bands) {  // it is important to duplicate st reference here, otherwise changes will affect input cube as well
        _st_ref->nx() = 1;
        _st_ref->ny() = 1;
        assert(_st_ref->nx() == 1 && _st_ref->ny() == 1);

        _chunk_size[0] = _in_cube->chunk_size()[0];
        _chunk_size[1] = 1;
        _chunk_size[2] = 1;

        // TODO: check for duplicate band, reducer pairs?

        for (uint16_t i = 0; i < reducer_bands.size(); ++i) {
            std::string reducerstr = reducer_bands[i].first;
            std::string bandstr = reducer_bands[i].second;
            if (!(reducerstr == "min" ||
                  reducerstr == "max" ||
                  reducerstr == "mean" ||
                  reducerstr == "median" ||
                  reducerstr == "count" ||
                  reducerstr == "var" ||
                  reducerstr == "sd" ||
                  reducerstr == "prod" ||
                  reducerstr == "sum"))
                throw std::string("ERROR in reduce_space_cube::reduce_space_cube(): Unknown reducer '" + reducerstr + "'");

            if (!(in->bands().has(bandstr))) {
                throw std::string("ERROR in reduce_space_cube::reduce_space_cube(): Input data cube has no band '" + bandstr + "'");
            }

            band b = in->bands().get(bandstr);
            if (in->size_x() > 1 || in->size_y() > 1) {
                b.name = b.name + "_" + reducerstr;  // Change name only if input is not yet reduced
            }
            _bands.add(b);
        }
    }

   public:
    ~reduce_space_cube() {}

    std::shared_ptr<chunk_data> read_chunk(chunkid_t id) override;

    /**
 * Combines all chunks and produces a single GDAL image
 * @param path path to output image file
 * @param format GDAL format (see https://www.gdal.org/formats_list.html)
 * @param co GDAL create options
     * @param p chunk processor instance, defaults to the current global configuration in config::instance()->get_default_chunk_processor()
 */

    nlohmann::json make_constructible_json() override {
        nlohmann::json out;
        out["cube_type"] = "reduce_space";
        out["reducer_bands"] = _reducer_bands;
        out["in_cube"] = _in_cube->make_constructible_json();
        return out;
    }

   private:
    std::shared_ptr<cube> _in_cube;
    std::vector<std::pair<std::string, std::string>> _reducer_bands;

    virtual void set_st_reference(std::shared_ptr<cube_st_reference> stref) override {
        // copy fields from st_reference type
        _st_ref->win() = stref->win();
        _st_ref->srs() = stref->srs();

        _st_ref->nx() = 1;
        _st_ref->ny() = 1;

        _st_ref->t0() = stref->t0();
        _st_ref->t1() = stref->t1();
        _st_ref->dt(stref->dt());
    }
};

#endif  // REDUCE_SPACE_H
