//
// LICENSE:
//
// Copyright (c) 2016 -- 2020 Fabio Pellacini
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//

#include <yocto/yocto_trace.h>
#include <yocto/yocto_commonio.h>
#include <yocto/yocto_sceneio.h>
#include <yocto/yocto_shape.h>

#include "ext/json.hpp"
#include "ext/filesystem.hpp"
#include "ext/earcut.hpp"

#include <atomic>
#include <deque>
#include <future> 
#include <iostream> 
#include <string> 
#include <unordered_map>
#include <time.h>

#include <typeinfo>


namespace cli = yocto::commonio;
namespace sio = yocto::sceneio;
namespace img = yocto::image;
namespace trc = yocto::trace;
namespace sfs = ghc::filesystem;
namespace shp = yocto::shape;

using namespace yocto::math;
using namespace std::string_literals;
using std::array;
using json = nlohmann::json;


int scale = 50; //10

class CityObject {
    public:
        std::string name;
        std::string type;
        std::string roof_shape;
        std::string colour;
        int level;
        float height;
        float roof_height;
        std::string historic;
        float thickness;

        std::vector<std::array<double,2>> coords;
        std::vector<std::array<double,2>> new_coords;
        std::vector<std::vector<std::array<double,2>>> holes;
        std::vector<std::vector<std::array<double,2>>> new_holes;  
};



class Coordinate {
    public:
        double x_minimum = __DBL_MAX__;
        double y_minimum = __DBL_MAX__;
        double x_maximum = __DBL_MIN__;
        double y_maximum = __DBL_MIN__;  

        void set_x_min(double x_min){ 
          if (x_minimum > x_min)
            x_minimum = x_min;
        }

        void set_y_min(double y_min){ 
          if (y_minimum > y_min)
            y_minimum = y_min;
        }

        void set_y_max(double y_max){ 
          if (y_maximum < y_max)
            y_maximum = y_max;
        }

        void set_x_max(double x_max){ 
          if (x_maximum < x_max)
            x_maximum = x_max;
        }

        void update(double x, double y){ 
          set_x_max(x);
          set_x_min(x);
          set_y_max(y);
          set_y_min(y); 
        }
};



// Application state
struct app_state {
  // loading options
  std::string geojson_filename  = "";
  std::string filename_save = "";

  // options
  trc::trace_params params = {};
  bool       add_skyenv = false;

  // scene
  trc::scene*              scene        = new trc::scene{};
  trc::camera*             camera       = nullptr;
  sio::model*              ioscene      = new sio::model{};
  std::vector<std::string> camera_names = {};

  // additional
  std::vector<CityObject> all_geometries;

};


//  --------------- FUNCTIONS --------------

bool check_high(nlohmann::json properties){
  nlohmann::json building_category = properties["building"];
  bool high_building = false;

  if ((building_category == "apartments") || (building_category == "residential") || (building_category == "tower") || (building_category == "hotel")) {
    high_building = true;
  }
  return high_building;
}


bool check_digit(std::string lev){
  bool digit = true;
  for (int i = 0; i < lev.size(); i++) { 
    //std::cout << typeid(lev[i]).name() << std::endl;
    if ((lev[i] >= 'a' && lev[i] <= 'z') || (lev[i] >= 'A' && lev[i] <= 'B') || lev[i] == ';' || lev[i] == ',')
      digit = false;
  }
  return digit;
}



bool check_int(std::string lev){
  bool integer = true;
  for (int i = 0; i < lev.size(); i++) { 
    if (lev[i] == '.'){
      integer = false;
    } 
  }
  return integer;
}


int generate_building_level(std::string footprint_type, nlohmann::json properties){
  int level = 1;
  float height = -1.0f;
  bool high_building = false;
  std::string::size_type sz;

  if(!properties["building:levels"].empty()){
    std::string lev = properties["building:levels"];  
    /*std::cout << "level" << std::endl;
    std::cout << lev << std::endl;*/
    bool digit = check_digit(lev);
    int n_levels_i = -1;
    float n_levels_f = -1.0f;

    if (digit){
      bool integer = check_int(lev);
      if (integer){
        n_levels_i = std::stoi(lev, &sz);
        level = (int) round(n_levels_i) + 1;
      } else {
        n_levels_f = std::stof(lev, &sz);
        level = (int) round(n_levels_f) + 1;
      }
    } else {
      level = 1;  // piano terra
    }

    //std::cout << "level" << std::endl;
    //std::cout << level << std::endl;
  }

  // Check if the building:height is given in the GeoJson file
  if (footprint_type == "building" && !properties["height"].empty()){
      std::string h = properties["height"];  
      bool digit = check_digit(h);
      if (digit){
        height = std::stof(h, &sz);
      }
  }

  if(footprint_type == "building" && !properties["building:height"].empty()){
      std::string h = properties["building:height"];  
      bool digit = check_digit(h);
      if (digit){
        height = std::stof(h, &sz);
      }
  }

  if (height > -1.0){
    level = int(float(height) / 3.2);
  }

  high_building = check_high(properties);
  if (footprint_type == "building" && !properties["building"].empty() && high_building){
    level = 3; 
  }
  //std::cout << level << std::endl;
  return level;
}


float generate_height(CityObject building, int scale){

  float height = 0.0001f;
  std::string::size_type sz;

  if (building.type == "building" && building.level > 0){

    height = (float) (building.level + (scale / 20.0)) / 20.0; //(float) (level + (scale / 20.0)) / 20.0;

  } else if (building.type == "water"){
    height = (float) 0.0001f;
  } else if (building.type == "highway"){
    height = (float) 0.0005f;
  } else if (building.type == "pedestrian"){
    height = (float) 0.0004f;
  }
  //std::cout << height << std::endl;
  
  return height;
}


float generate_roof_height(std::string roof_h, int scale){
  float roof_height = 0.109f;
  std::string::size_type sz;

  if (roof_h != "null"){ 
    float roof_hei = (float) std::stof(roof_h, &sz);
    roof_height = (float) roof_hei / scale;
  }
  
  return roof_height;
}


bool check_grass_type(std::string building_type){
  bool grass_area = false;
  if(building_type == "park" || building_type == "pitch" || building_type == "garden" || building_type == "playground" || building_type == "greenfield" || building_type == "scrub" || building_type == "heath" || building_type == "farmyard" || building_type == "grass" || building_type == "farmland" || building_type == "village_green" || building_type == "meadow" || building_type == "orchard" || building_type == "vineyard" || building_type == "recreation_ground" || building_type == "grassland"){
    grass_area = true;
  }
  return grass_area;
}


bool check_pedestrian(nlohmann::json properties) {
  nlohmann::json highway_category = properties["highway"];
  bool is_pedestrian = false;

  if ((highway_category == "footway") || (highway_category == "pedestrian") || (highway_category == "track") || (highway_category == "steps") || (highway_category == "path") || (highway_category == "living_street") || (highway_category == "pedestrian_area") || (highway_category == "pedestrian_line")) {
    is_pedestrian = true;
  }
  return is_pedestrian;  
}


vec3f get_color(std::string type, bool grass_type){
  vec3f color = {0.725, 0.71, 0.68};  // floor color
  if (type == "building"){
    color = vec3f{0.79, 0.74, 0.62};
  } else if (type == "highway"){
    color = vec3f{0.26, 0.26, 0.28};
  } else if (type == "pedestrian"){
    color = vec3f{0.45, 0.4, 0.27}; //color = vec3f{0.82, 0.82, 0.82};
  } else if (type == "water"){
    color = vec3f{0.72, 0.95, 1.0};
  } else if (type == "sand"){
    color = vec3f{0.69, 0.58, 0.43};
  } else if (type == "forest"){
    color = vec3f{0.004, 0.25, 0.16}; 
  } else if (grass_type)
    color = vec3f{0.337, 0.49, 0.274};
  return color;
}


vec3f get_building_color(std::string building_color) {
  vec3f color;
  if (building_color == "yellow"){
    color = vec3f{0.882,0.741,0.294};
  } else if (building_color == " light yellow"){
    color = vec3f{0.922,0.925,0.498};
  } else if (building_color == "brown"){
    color = vec3f{0.808,0.431,0.271};
  } else if (building_color == "light brown"){
    color = vec3f{0.8,0.749,0.596};
  } else if (building_color == "light orange"){
    color = vec3f{0.933,0.753,0.416};
  } else { // white
    color = vec3f{1.0,1.0,1.0}; //white
  }
  return color;
}



bool create_city_from_json(sio::model* scene, std::vector<CityObject> all_geometries){
  scene->name             = "City";
  auto camera             = add_camera(scene);
  camera->frame           = frame3f{vec3f{-0.028f, 0.0f, 1.0f},
      vec3f{0.764f, 0.645f, 0.022f}, vec3f{-0.645f, 0.764f, -0.018f},
      vec3f{-13.032f, 16.750f, -1.409f}};
  camera->lens            = 0.035;
  camera->aperture        = 0.0;
  camera->focus           = 3.9;
  camera->film            = 0.024;
  camera->aspect          = 1;
  auto floor              = sio::add_complete_object(scene, "floor");
  auto floor_size         = 60.0f;
  floor->shape->positions = {{-floor_size, 0, floor_size},
      {floor_size, 0, floor_size}, {floor_size, 0, -floor_size},
      {-floor_size, 0, -floor_size}};
  floor->shape->triangles = {{0, 1, 2}, {2, 3, 0}};
  floor->material->color  = {0.725, 0.71, 0.68};

  add_sky(scene);

  std::string error = ""s;



  // Load 3D models (trees)

  // standard tree
  std::string path_standard = "./shapes/tree/standard.ply";
  auto shape_standard = add_shape(scene,"standard"); 
  if (!shp::load_shape(path_standard, shape_standard->points, shape_standard->lines, shape_standard->triangles,
          shape_standard->quads, shape_standard->positions, shape_standard->normals, shape_standard->texcoords,
          shape_standard->colors, shape_standard->radius, error))
    return false;
  

  // palm tree
  std::string path_palm = "./shapes/tree/palm.ply";
  auto shape_palm = add_shape(scene,"palm");
  if (!shp::load_shape(path_palm, shape_palm->points, shape_palm->lines, shape_palm->triangles,
          shape_palm->quads, shape_palm->positions, shape_palm->normals, shape_palm->texcoords,
          shape_palm->colors, shape_palm->radius, error))
    return false;


  // pine tree
  std::string path_pine = "./shapes/tree/pine.ply";
  auto shape_pine = add_shape(scene,"pine");
  if (!shp::load_shape(path_pine, shape_pine->points, shape_pine->lines, shape_pine->triangles,
          shape_pine->quads, shape_pine->positions, shape_pine->normals, shape_pine->texcoords,
          shape_pine->colors, shape_pine->radius, error))
    return false;


  // cypress tree
  std::string path_cypress = "./shapes/tree/cypress.ply";
  auto shape_cypress = add_shape(scene,"cypress");
  if (!shp::load_shape(path_cypress, shape_cypress->points, shape_cypress->lines, shape_cypress->triangles,
          shape_cypress->quads, shape_cypress->positions, shape_cypress->normals, shape_cypress->texcoords,
          shape_cypress->colors, shape_cypress->radius, error))
    return false;


  // oak tree
  std::string path_oak = "./shapes/tree/oak.ply";
  auto shape_oak = add_shape(scene,"oak");
  if (!shp::load_shape(path_oak, shape_oak->points, shape_oak->lines, shape_oak->triangles,
          shape_oak->quads, shape_oak->positions, shape_oak->normals, shape_oak->texcoords,
          shape_oak->colors, shape_oak->radius, error))
    return false;


  // Load textures

  // buidling texture1
  auto texture_1 = sio::add_texture(scene, "texture1");
  std::string path_text_1 = "./textures/1.jpg";
  auto build_texture_1 = load_image(path_text_1, texture_1->colorf, error);

  // buidling texture2
  auto texture_2 = sio::add_texture(scene, "texture2");
  std::string path_text_2 = "./textures/2.jpg";
  auto build_texture_2 = load_image(path_text_2, texture_2->colorf, error);

  // buidling texture3
  auto texture_3 = sio::add_texture(scene, "texture3");
  std::string path_text_3 = "./textures/3.jpg";
  auto build_texture_3 = load_image(path_text_3, texture_3->colorf, error);

  // buidling texture4
  auto texture_4 = sio::add_texture(scene, "texture4");
  std::string path_text_4 = "./texture/4.jpg";
  auto build_texture_4 = load_image(path_text_4, texture_4->colorf, error);

  // buidling texture5
  auto texture_5 = sio::add_texture(scene, "texture5");
  std::string path_text_5 = "./textures/5.jpg";
  auto build_texture_5 = load_image(path_text_5, texture_5->colorf, error);

  // buidling texture6
  auto texture_6 = sio::add_texture(scene, "texture6");
  std::string path_text_6 = "./textures/6.jpg";
  auto build_texture_6 = load_image(path_text_6, texture_6->colorf, error);

  // buidling texture7
  auto texture_7 = sio::add_texture(scene, "texture7");
  std::string path_text_7 = "./textures/7.jpg";
  auto build_texture_7 = load_image(path_text_7, texture_7->colorf, error);

  // buidling texture8
  auto texture_8 = sio::add_texture(scene, "texture8");
  std::string path_text_8 = "./textures/8.jpg";
  auto build_texture_8 = load_image(path_text_8, texture_8->colorf, error);

  // buidling texture8_11
  auto texture_8_11 = sio::add_texture(scene, "texture8_11");
  std::string path_text_8_11 = "./textures/8_11.jpg";
  auto build_texture_8_11 = load_image(path_text_8_11, texture_8_11->colorf, error);

  // buidling texture10_41
  auto texture_10_41 = sio::add_texture(scene, "texture10_41");
  std::string path_text_10_41 = "./textures/10_41.jpg";
  auto build_texture_10_41 = load_image(path_text_10_41, texture_10_41->colorf, error);

  // buidling texture40_71
  auto texture_40_71 = sio::add_texture(scene, "texture40_71");
  std::string path_text_40_71 = "./textures/40_71.jpg";
  auto build_texture_40_71 = load_image(path_text_40_71, texture_40_71->colorf, error);

  // buidling texture70_101
  auto texture_70_101 = sio::add_texture(scene, "texture70_101");
  std::string path_text_70_101 = "./textures/70_101.jpg";
  auto build_texture_70_101 = load_image(path_text_70_101, texture_70_101->colorf, error);

  // buidling texturemore_101
  auto texture_more_101 = sio::add_texture(scene, "texturemore_101");
  std::string path_text_more_101 = "./textures/more_101.jpg";
  auto build_texture_more_101 = load_image(path_text_more_101, texture_more_101->colorf, error);
  
  // buidling texture_colosseo
  auto texture_colosseo = sio::add_texture(scene, "texture_colosseo");
  std::string path_text_colosseo = "./textures/colosseo.jpg";
  


  // Check if exists the element of interest
  bool exist_element = false;
  for(auto& building_geometry : all_geometries){
    auto building_type = building_geometry.type;
    bool grass_area = check_grass_type(building_type);
    if(building_geometry.type == "building" || building_geometry.type == "water" || building_geometry.type == "highway" || building_geometry.type == "pedestrian" || building_geometry.type == "forest" || grass_area || building_geometry.type == "standard" || building_geometry.type == "palm" || building_geometry.type == "pine" || building_geometry.type == "oak" || building_geometry.type == "cypress"){    
      exist_element = true;  
    }
  }

  if (exist_element) { 
    using Coord = double;
    using N     = int32_t;
    using Point = std::array<Coord, 2>;
    double all_tempo;
    double all_triangles;
    double all_quads;
    double all_elements;
    double all_elements_with_hole;

    for (auto& element : all_geometries) {
      auto name = element.name;

      std::string type_s = element.type;
      std::string type_roof = "null";
      std::string historic = "no";

      if (element.roof_shape != "null")
        type_roof = element.roof_shape;
        
      if (element.historic != "no")
        historic = element.historic;
        
      if (type_s == "standard"){
        auto tree = add_complete_object(scene, name);         
        vec3f coord = {};

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = 0.0;
          coord.z = elem[1];

          auto x = coord.x + 0.09f;
          auto z = coord.z + 0.09f;

          // create TREE object
          tree->shape = shape_standard;
          tree->material->color = {0.002, 0.187, 0.008};
          tree->frame = frame3f{vec3f{1.0f, 0.0f, 0.0f},
                vec3f{0.0f, 1.0f, 0.0f}, vec3f{0.0f, 0.0f, 1.0f},
                vec3f{x, coord.y, z}};
          coord = {};
          continue;
        }

      } else if (type_s == "palm"){
        auto tree = add_complete_object(scene, name);     
        vec3f coord = {};

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = 0.0;
          coord.z = elem[1];

          // create TREE object   
          tree->shape = shape_palm;
          tree->material->color = {0.224, 0.5, 0.06};
          tree->frame = frame3f{vec3f{1.0f, 0.0f, 0.0f},
              vec3f{0.0f, 1.0f, 0.0f}, vec3f{0.0f, 0.0f, 1.0f},
              vec3f{coord.x, coord.y, coord.z}};
             
          coord = {};
          continue;
        }
      } else if(type_s == "cypress"){
        auto tree = add_complete_object(scene, name); 
        vec3f coord = {};

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = 0.0;
          coord.z = elem[1];

          // create TREE object
          tree->shape = shape_cypress;
          tree->material->color = {0.019, 0.175, 0.039};
          tree->frame = frame3f{vec3f{1.0f, 0.0f, 0.0f},
              vec3f{0.0f, 1.0f, 0.0f}, vec3f{0.0f, 0.0f, 1.0f},
              vec3f{coord.x, coord.y, coord.z}};
              
          coord = {};
          continue;
        }
      } else if (type_s == "oak"){
        auto tree = add_complete_object(scene, name); 
        vec3f coord = {};

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = 0.0;
          coord.z = elem[1];

          // create TREE object OAK
          tree->shape = shape_oak;
          tree->material->color = {0.084,0.193,0.005};
          tree->frame = frame3f{vec3f{1.0f, 0.0f, 0.0f},
              vec3f{0.0f, 1.0f, 0.0f}, vec3f{0.0f, 0.0f, 1.0f},
              vec3f{coord.x, coord.y, coord.z}};

          coord = {};
          continue;
        }
      } else if (type_s == "pine"){
        auto tree = add_complete_object(scene, name); 
        vec3f coord = {};

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = 0.0;
          coord.z = elem[1];

          // create TREE object           
          tree->shape = shape_pine;
          tree->material->color = {0.145,0.182,0.036};
          tree->frame = frame3f{vec3f{1.0f, 0.0f, 0.0f},
              vec3f{0.0f, 1.0f, 0.0f}, vec3f{0.0f, 0.0f, 1.0f},
              vec3f{coord.x, coord.y, coord.z}};
            
          coord = {};
          continue;
        }         
      } else { //if (type_s == "water"){
        all_elements += 1;

        /*clock_t start,end;
        double tempo;
        // ---- TIME STARTS ---
        start=clock();*/

        std::vector<std::vector<Point>> polygon;

        auto build = sio::add_complete_object(scene, name);
        std::vector<vec3i> triangles;
        std::vector<vec3f> positions;

        std::vector<Point> vect_building;
        vec3f              coord    = {};
        float              height   = -1.0f;
        float              roof_height = -1.0f;
        int                level    = 0;
        std::string        type    = "";

        std::string::size_type sz;

        type = element.type;

        if (element.level > 0){
          level = element.level;
        }

        height = element.height;

        for (auto& elem : element.new_coords) {
          coord.x = elem[0];
          coord.y = height;
          coord.z = elem[1];

          positions.push_back(coord);
          vect_building.push_back({coord.x, coord.z});

          coord = {};
          continue;
        }
        polygon.push_back(vect_building);

        std::vector<Point> vect_hole;
        coord = {};
            
        for (auto& list : element.new_holes) {
          for (auto& h : list) {
            coord.x = h[0];
            coord.z = h[1];
            coord.y = height;

            positions.push_back(coord);
            vect_hole.push_back({coord.x, coord.z});
            coord = {};         
          }

          polygon.push_back(vect_hole);
          vect_hole = {}; 
        }
              
        int num_holes = element.new_holes.size();
        if (num_holes > 0){
          all_elements_with_hole += 1;
        }

        bool color_given = false;
        if (element.colour != "null")
          color_given = true;

        bool grass_area = check_grass_type(element.type);  
        auto color = get_color(type, grass_area); //vec3f{0.79, 0.74, 0.62};

        if (type_roof == "flat" && num_holes == 0){
          type_roof = "gabled";
        } else if (name == "building_relation_1834818") { //colosseo
          build->material->color = vec3f{0.725,0.463,0.361};
        } else if (type == "building" && level < 3 && historic != "no") {
          build->material->color = vec3f{0.538,0.426,0.347}; //vec3f{0.402,0.319,0.261}; // light brown
        } else if (historic == "yes" && color_given){
          std::string building_color = element.colour;
          vec3f build_color = get_building_color(building_color);
          build->material->color = build_color;
        } else {
          build->material->color = color;
        }

        std::vector<vec3f> _polygon;
        for (auto point : positions) {
          _polygon.push_back(point);
        }

        std::vector<N> indices = mapbox::earcut<N>(polygon);

        for (int k = 0; k < indices.size() - 2; k += 3) {
          triangles.push_back({indices[k], indices[k + 1], indices[k + 2]});
        }
        all_triangles += triangles.size();

        // Water characteristics
        if (type == "water") {
          build->material->specular = 1.0f;
          build->material->transmission = 0.99f;
          build->material->metallic = 0.8f;
          build->material->roughness = 0.1f;
        }

        // Road characteristics
        if (type == "highway") {
          build->material->roughness = 0.9f;
          build->material->specular  = 0.7f;
        }

        // Filling buildings
        if (type == "building") {
          auto build2             = sio::add_complete_object(scene, name + "_1");
          build2->material->color = color;
          std::vector<vec3f> _polygon2;
          for (auto point : positions) {
            _polygon2.push_back(point);
          }

          // Quads on the building sides
          std::vector<vec4i> quads;
          for (int i = 0; i < positions.size(); i++) {
            auto prev_index = i - 1;
            if (prev_index == -1) {
              prev_index = positions.size() - 1;
            }
            auto index = (int)_polygon2.size();
            _polygon2.push_back({positions[i].x, 0, positions[i].z});
            auto index_2 = (int)_polygon2.size();
            _polygon2.push_back({positions[prev_index].x, 0, positions[prev_index].z});

            quads.push_back({prev_index, i, index, index_2});
          }
          all_quads += quads.size();

          build2->material->color = color;

          if (historic == "yes"){
            if (name == "building_relation_1834818") { // colosseo 
              auto build_texture_colosseo = load_image(path_text_colosseo, texture_colosseo->colorf, error);
              build2->material->color_tex = texture_colosseo;
            } else if (element.colour != "null"){
              std::string building_color = element.colour;
              vec3f build_color = get_building_color(building_color);
              build2->material->color = build_color;
            } else {
              build2->material->color = color;
            }
          } else {
            if (level == 1)
              build2->material->color_tex =  texture_1;
            else if (level == 2)
              build2->material->color_tex = texture_2;
            else if (level == 3)
              build2->material->color_tex = texture_3;
            else if (level == 4)
              build2->material->color_tex = texture_4;
            else if (level == 5)
              build2->material->color_tex = texture_5;
            else if (level == 6)
              build2->material->color_tex = texture_6;
            else if (level == 7)
              build2->material->color_tex = texture_7;
            else if (level == 8)
              build2->material->color_tex = texture_8;
            else if (level > 8 && level < 11)
              build2->material->color_tex = texture_8_11;
            else if (level > 10 && level < 41)
              build2->material->color_tex = texture_10_41;    
            else if (level > 40 && level < 71)
              build2->material->color_tex = texture_40_71;  
            else if (level > 70 && level < 101)
              build2->material->color_tex = texture_70_101;  
            else if (level > 101)
              build2->material->color_tex = texture_more_101;        
          }

          build2->shape->positions = _polygon2;
          build2->shape->quads = quads;
        }

        build->shape->positions = _polygon;
        build->shape->triangles = triangles;
        
        // Gabled roof
        if (type_roof == "gabled") {
          std::vector<std::vector<Point>> polygon_roof;

          auto               roof = sio::add_complete_object(scene, name);
          std::vector<vec3i> triangles_roof;
          std::vector<vec3f> positions_roof;

          std::vector<Point> vect_roof;
          float              roof_height = -1.0f;
          vec3f              coord    = {};
          float              height   = -1.0f;

          height = element.height;
          roof_height = element.roof_height;

          float centroid_x = 0.0f;
          float centroid_y = 0.0f; 
          int num_vert = (int) element.new_coords.size();
          int num_holes = element.new_holes.size();
          
          if (num_holes == 0) {
            for (auto& elem : element.new_coords) {
              coord.x = (double) elem[0];
              coord.y = height;
              coord.z = (double) elem[1];

              positions_roof.push_back(coord);
              vect_roof.push_back({coord.x, coord.z});

              centroid_x += coord.x;
              centroid_y += coord.z;

              coord = {};
              continue;
            }

            centroid_x = centroid_x / num_vert;
            centroid_y = centroid_y / num_vert;
            
            polygon_roof.push_back(vect_roof);

            auto roof_color = vec3f{0.351,0.096,0.091}; // brown/red
            roof->material->color = roof_color; 

            std::vector<vec3f> _polygon_roof;
            for (auto point : positions_roof) {
              _polygon_roof.push_back(point);
            }

            std::vector<N> indices_roof = mapbox::earcut<N>(polygon_roof);

            for (int k = 0; k < indices_roof.size() - 2; k += 3) {
              triangles_roof.push_back({indices_roof[k], indices_roof[k + 1], indices_roof[k + 2]});
            }

            // Filling roofs
            auto roof2             = sio::add_complete_object(scene, name + "_roof");
            roof2->material->color = roof_color;
            std::vector<vec3f> _polygon2_roof;
            for (auto point : positions_roof) {
              _polygon2_roof.push_back(point);
            }
            std::vector<vec3i> triangles2_roof;
            for (int i = 0; i < positions_roof.size(); i++) {
              auto prev_index = i - 1;
              if (prev_index == -1) {
                prev_index = positions_roof.size() - 1;
              }
              auto total_height = height + roof_height;
              auto index = (int)_polygon2_roof.size();
              _polygon2_roof.push_back({centroid_x, total_height, centroid_y});
              auto index_2 = (int)_polygon2_roof.size();
              _polygon2_roof.push_back({centroid_x, total_height, centroid_y});
              triangles2_roof.push_back({prev_index, i, index});
              triangles2_roof.push_back({index, index_2, prev_index});  
            }

            roof2->shape->positions = _polygon2_roof;
            roof2->shape->triangles = triangles2_roof;

            roof->shape->positions = _polygon_roof;
            roof->shape->triangles = triangles_roof;
          } 
        }
        /*end=clock();
        tempo=((double)(end-start))/CLOCKS_PER_SEC;  // time in seconds
        all_tempo += tempo;*/

      } 
    } 
    /*std::cout << "Time" << std::endl;
    std::cout << all_tempo << std::endl;
    std::cout << "Triangles" << std::endl;
    std::cout << all_triangles << std::endl;
    std::cout << "Quads" << std::endl;
    std::cout << all_quads << std::endl;
    std::cout << "Elements" << std::endl;
    std::cout << all_elements << std::endl;
    std::cout << "Elements with hole" << std::endl;
    std::cout << all_elements_with_hole << std::endl;*/
  }
  return true;
}


std::vector<std::array<double,2>> compute_area(double x, double next_x, double y, double next_y, double road_thickness) {
  
 std::vector<std::array<double,2>> line_1 = {{next_x + road_thickness, next_y + road_thickness}, {next_x - road_thickness, next_y - road_thickness},
                      {x - road_thickness, y - road_thickness}, {x + road_thickness, y + road_thickness}};

  std::vector<double> vec_x = {};
  std::vector<double> vec_y = {};


  for (auto& couple : line_1){
    vec_x.push_back(couple[0]);
    vec_y.push_back(couple[1]);
  }

  std::vector<double> shifted_vec_x = vec_x;
  std::vector<double> shifted_vec_y = vec_y;

  
  std::rotate(shifted_vec_x.begin(), shifted_vec_x.begin()+3, shifted_vec_x.end());
  std::rotate(shifted_vec_y.begin(), shifted_vec_y.begin()+3, shifted_vec_y.end());
  

  double sum_first = 0.0f;
  for (int i = 0; i < vec_x.size(); i++) {
    double first_prod = vec_x[i] * shifted_vec_y[i];
    sum_first += first_prod;
  }


  double sum_second = 0.0f;
  for (int i = 0; i < vec_y.size(); i++) {
    double second_prod = vec_y[i] * shifted_vec_x[i];
    sum_second += second_prod;
  }

  float area_1 = (float) (0.5f * fabs(sum_first - sum_second));


  // -----------
  std::vector<std::array<double,2>> line_2 = {{next_x + road_thickness, next_y}, {next_x - road_thickness, next_y},
                     {x - road_thickness, y}, {x + road_thickness, y}};
 
  std::vector<double> vec_x_2 = {};
  std::vector<double> vec_y_2 = {};

  for (auto& couple : line_2){
    vec_x_2.push_back(couple[0]);
    vec_y_2.push_back(couple[1]);
  }

  std::vector<double> shifted_vec_x_2 = vec_x_2;
  std::vector<double> shifted_vec_y_2 = vec_y_2;
  
  std::rotate(shifted_vec_x_2.begin(), shifted_vec_x_2.begin()+3, shifted_vec_x_2.end());
  std::rotate(shifted_vec_y_2.begin(), shifted_vec_y_2.begin()+3, shifted_vec_y_2.end());

  double sum_first_2 = 0.0f;
  for (int i = 0; i < vec_x_2.size(); i++) {
    double first_prod_2 = vec_x_2[i] * shifted_vec_y_2[i];
    sum_first_2 += first_prod_2;
  }

  double sum_second_2 = 0.0f;
  for (int i = 0; i < vec_y_2.size(); i++) {
    double second_prod_2 = vec_y_2[i] * shifted_vec_x_2[i];
    sum_second_2 += second_prod_2;
  }

  float area_2 = (float) (0.5f * fabs(sum_first_2 - sum_second_2));


  // -----------
  std::vector<std::array<double,2>> line_3 = {{next_x, next_y + road_thickness}, {next_x, next_y - road_thickness},
                     {x, y - road_thickness}, {x, y + road_thickness}};

  std::vector<double> vec_x_3 = {};
  std::vector<double> vec_y_3 = {};

  for (auto& couple : line_3){
    vec_x_3.push_back(couple[0]);
    vec_y_3.push_back(couple[1]);
  }

  std::vector<double> shifted_vec_x_3 = vec_x_3;
  std::vector<double> shifted_vec_y_3 = vec_y_3;
  
  std::rotate(shifted_vec_x_3.begin(), shifted_vec_x_3.begin()+3, shifted_vec_x_3.end());
  std::rotate(shifted_vec_y_3.begin(), shifted_vec_y_3.begin()+3, shifted_vec_y_3.end());

  double sum_first_3 = 0.0f;
  for (int i = 0; i < vec_x_3.size(); i++) {
    double first_prod_3 = vec_x_3[i] * shifted_vec_y_3[i];
    sum_first_3 += first_prod_3;
  }

  double sum_second_3 = 0.0f;
  for (int i = 0; i < vec_y_3.size(); i++) {
    double second_prod_3 = vec_y_3[i] * shifted_vec_x_3[i];
    sum_second_3 += second_prod_3;
  }

  float area_3 = (float) (0.5f * fabs(sum_first_3 - sum_second_3));


  if (area_2 > area_1){
    if (area_3 > area_2){
      return line_3;
    } else {
      return line_2;
    }
  } else {
    if (area_3 > area_1){
      return line_3;
    } else {
      return line_1;
    }
  }
  

  /*if (area_2 > area_1)
    return line_2;    
  return line_1;*/
  
}

float get_thickness(std::string type){
  float thickness = 0.0001;
  if (type == "pedestrian") {
    thickness = 0.00005;
  } else if (type == "water") { // MultiLineString
    thickness = 1.0;
  }
  return thickness;
}


CityObject assign_type(CityObject building, nlohmann::json properties){
  if (!properties["building"].empty()){
    building.type = "building";
    if (!properties["roof:shape"].empty()){
      std::string roof_shape = properties["roof:shape"];
      if (roof_shape == "gabled" || roof_shape == "onion" || roof_shape == "pyramid")
        building.roof_shape = "gabled";
      else if (roof_shape == "flat")
        building.roof_shape = "flat";
    }
    
    if (!properties["roof:height"].empty()){
      std::string roof_h = properties["roof:height"];
      double roof_height = generate_roof_height(roof_h, scale);
      building.roof_height = roof_height;
    }
        
    if (!properties["historic"].empty()){
      building.historic = "yes";
      if (!properties["building:colour"].empty()) {
        std::string build_colour = properties["building:colour"]; 
        building.colour = build_colour;
      }
    }

    if (!properties["tourism"].empty()){
      std::string tourism = properties["tourism"];
      if (tourism == "attraction"){
        building.historic = "yes";
        if (!properties["building:colour"].empty()) {
          std::string build_colour = properties["building:colour"]; 
          building.colour = build_colour;
        }
      }
    }
  }

  else if (!properties["water"].empty()){
    building.type = "water";
  }

  else if (!properties["landuse"].empty()){
    std::string landuse = properties["landuse"];
    building.type = landuse;
  }

  else if (!properties["natural"].empty()){
    std::string natural = properties["natural"];
    if (natural == "wood") {
      building.type = "forest";
    } else {
      building.type = natural;
    }
  } 

  else if (!properties["leisure"].empty()){
    std::string leisure = properties["leisure"];
    building.type = leisure;
  } 
          
  else if (!properties["highway"].empty()){
    bool pedestrian = check_pedestrian(properties);
    if(pedestrian){
      building.type = "pedestrian";
    } else {
      building.type = "highway";
    }
  }

  else {
    building.type = "null";
  }

  return building;
}




std::vector<CityObject> assign_tree_type(CityObject point, nlohmann::json properties, std::vector<CityObject> all_buildings){
  if (!properties["natural"].empty()){
    std::string point_type_nat = properties["natural"];
    if (point_type_nat == "tree") {
      if (!properties["type"].empty()) {
        std::string type_tree = properties["type"];
        if (type_tree == "palm") {
          point.type = "palm";
          all_buildings.push_back(point);
        } else if (type_tree == "pine") {
          point.type = "pine";
          all_buildings.push_back(point);
        } else if (type_tree == "cypress") {
          point.type = "cypress";
          all_buildings.push_back(point);
        } else {
          point.type = "standard";
          all_buildings.push_back(point);
        }
      } else if (!properties["tree"].empty()){
        point.type = "standard";
        all_buildings.push_back(point);
      } else if (!properties["genus"].empty()){
        std::string genus_tree = properties["genus"];
        if (genus_tree == "Quercus") {
          point.type = "oak";
          all_buildings.push_back(point);
        } else if (genus_tree == "Cupressus") {
          point.type = "cypress";
          all_buildings.push_back(point);
        } else if (genus_tree == "Pinus") {
          point.type = "pine";
          all_buildings.push_back(point);
        } else {
          point.type = "standard";
          all_buildings.push_back(point);
        }
      }
      else {
        point.type = "standard";
        all_buildings.push_back(point);
      }
    }
  }

  else {
    point.type = "null";
  }

  return all_buildings;
}


bool check_valid_type(CityObject building, nlohmann::json properties) {
  bool valid = false;

  bool grass_area = check_grass_type(building.type);
  if (building.type == "building" || building.type == "water" || building.type == "sand" || grass_area || building.type == "highway" || building.type == "pedestrian" || building.type == "forest")
    valid = true;

  return valid;
}



std::pair<std::vector<CityObject>, Coordinate> data_analysis(nlohmann::json geojson_file, std::vector<CityObject> all_buildings, Coordinate class_coord){
  for (auto &feature : geojson_file["features"]){
    auto geometry = feature["geometry"];
    auto properties = feature["properties"];
    std::string id = properties["@id"];
    std::replace( id.begin(), id.end(), '/', '_'); // replace all '/' to '_'
    int count_list = 0;


    if (geometry["type"] == "Polygon"){
      auto building = CityObject();

      building = assign_type(building, properties);
      std::string footprint_type = building.type;

      if (footprint_type == "null"){
        continue;
      }

      int level = generate_building_level(footprint_type, properties);
      
      std::vector<std::array<double,2>> couple;
      std::vector<std::vector<std::array<double,2>>> list_holes;
      std::vector<std::array<double,2>> list_coordinates = {};

      int num_lists = geometry["coordinates"].size();
      for(auto &list_coords : geometry["coordinates"]){
        
        if(count_list == 0){ // outer polygon 
          building.level = level;

          for(auto& coord : list_coords){
            double x = (double) coord[0];
            double y = (double) coord[1];
            class_coord.update(x, y);
            std::array<double,2> arr = {x,y};
            list_coordinates.push_back(arr);
          }

          building.coords = list_coordinates;

          std::string name = id;
          std::string build_name = "building_" + name; 
          building.name = build_name;
          
          count_list++;
        }
        else {  // analysis of building holes 

          for(auto& coord : list_coords){
            double x = (double) coord[0];
            double y = (double) coord[1];
            couple.push_back({x,y});
          }
          list_holes.push_back(couple);
          couple = {};

          count_list++;
        }
        //std::cout << count << std::endl;
        
        if (count_list == num_lists){         
          building.holes = list_holes;

          bool valid_type = check_valid_type(building, properties);
          if (valid_type)
            all_buildings.push_back(building);
        }
      }
      count_list = 0;

    } else if (geometry["type"] == "MultiPolygon") { 
      auto building = CityObject();
      
      building = assign_type(building, properties);
      std::string footprint_type = building.type;

      if (footprint_type == "null"){
        continue;
      }

      int level = generate_building_level(footprint_type, properties);
      
      std::vector<std::array<double,2>> couple;
      std::vector<std::vector<std::array<double,2>>> list_holes;
      std::vector<std::array<double,2>> list_coordinates = {};

      for(auto &multi_pol : geometry["coordinates"]){
        int num_lists = multi_pol.size();
        for(auto &list_coords : multi_pol){
          //std::cout << geometry["coordinates"].size() << std::endl;
          
          if(count_list == 0){ // outer polygon 
            building.level = level;

            for(auto& coord : list_coords){
              double x = (double) coord[0];
              double y = (double) coord[1];
              class_coord.update(x, y);
              class_coord.update(x, y);
              std::array<double,2> arr = {x,y};
              list_coordinates.push_back(arr);
            }

            building.coords = list_coordinates;

            std::string name = id;
            building.name = "building_" + name; 

            count_list++;
          }
          else {  // analysis of building holes

            for(auto& coord : list_coords){
              double x = (double) coord[0];
              double y = (double) coord[1];
              couple.push_back({x,y});
            }
            list_holes.push_back(couple);
            couple = {};

            count_list++;
          }
          
          if (count_list == num_lists){
            building.holes = list_holes;
            bool valid_type = check_valid_type(building, properties);
            if (valid_type)
              all_buildings.push_back(building);
          }
        }
        count_list = 0;     
      }
   
    } else if (geometry["type"] == "LineString") { 
      int cont = 0;
      for (int i = 0; i < geometry["coordinates"].size()-1; i++) { 
        auto coord_i = geometry["coordinates"][i];
        auto coord_i_next = geometry["coordinates"][i+1];

        double x = (double) coord_i[0];
        double y = (double) coord_i[1];
        double next_x = (double) coord_i_next[0];
        double next_y = (double) coord_i_next[1];
        double line_thickness = 0.00005f;

        auto area = compute_area(x, next_x, y, next_y, line_thickness);

        auto line = CityObject();

        std::string name = id;
        line.name = "line_" + name + std::to_string(cont);
        cont++;
        
        if (!properties["highway"].empty()){
          bool pedestrian = check_pedestrian(properties);
          if(pedestrian){
            line.type = "pedestrian";
          } else {
            line.type = "highway";
          }
        }

        else if (!properties["natural"].empty()){
          std::string natural = properties["natural"];
          line.type = natural;
        }    
        else {
          continue;
        }

        line.thickness = get_thickness(line.type); 
        line.coords = area;
        all_buildings.push_back(line);

        for(auto& coord : area){
          double x = (double) coord[0];
          double y = (double) coord[1];
          class_coord.update(x, y);
        }
      }

    } else if (geometry["type"] == "MultiLineString") {
      
      int cont = 0;
      for (auto& list_line : geometry["coordinates"]){
        for (int i = 0; i < list_line.size()-1; i++) { 
          auto coord_i = list_line[i];
          auto coord_i_next = list_line[i+1];

          double x = (double) coord_i[0];
          double y = (double) coord_i[1];
          double next_x = (double) coord_i_next[0];
          double next_y = (double) coord_i_next[1];
          double line_thickness = 0.0004f; 

          auto area = compute_area(x, next_x, y, next_y, line_thickness);

          auto line = CityObject();

          std::string name = id;
          line.name = "multiline_" + name + std::to_string(cont);
          cont++;

          if (!properties["waterway"].empty()){
            line.type = "water";
          }
          else {
            continue;
          }

          line.thickness = line_thickness; 
          line.coords = area;
          all_buildings.push_back(line);

          for(auto& coord : area){
            double x = (double) coord[0];
            double y = (double) coord[1];
            class_coord.update(x, y);
          }
        }
      }
    
    } else if (geometry["type"] == "Point"){
      std::vector<std::array<double,2>> points;
      points.push_back(geometry["coordinates"]);

      auto point = CityObject(); 

      std::string name = id;
      point.name = "point_" + name;
      point.coords = points;

      all_buildings = assign_tree_type(point, properties, all_buildings);

      if (point.type == "null"){
        continue;
      }

      for(auto& coord : points){
        double x = (double) coord[0];
        double y = (double) coord[1];
        class_coord.update(x, y);
      }
    } 
  }  
  //std::cout << all_buildings << std::endl;
  return {all_buildings, class_coord};  
}



std::vector<CityObject> generate_new_coordinates(nlohmann::json geojson_file, std::vector<CityObject> all_buildings, Coordinate class_coord){
  std::pair<std::vector<CityObject>, Coordinate> gen_out;

  gen_out = data_analysis(geojson_file, all_buildings, class_coord);
  all_buildings = gen_out.first;
  class_coord = gen_out.second;


  std::vector<CityObject> all_objects = {};

  // Scale the CityObject outer polygon in the scene
  for (auto& building_geometry : all_buildings){
    float height = generate_height(building_geometry, scale);
    building_geometry.height = height;

    std::vector<std::array<double,2>> new_coords = {};
    std::vector<std::vector<std::array<double,2>>> new_holes = {};


    for (auto& couple : building_geometry.coords){
      double x = (double) couple[0];
      double y = (double) couple[1];

      double new_x = (x - class_coord.x_minimum) / (class_coord.x_maximum - class_coord.x_minimum) * scale - (
                      scale / 2);
      double new_y = (y - class_coord.y_minimum) / (class_coord.y_maximum - class_coord.y_minimum) * scale - (
                      scale / 2);       

      std::array<double,2> arr = {new_x, new_y};
      new_coords.push_back(arr);
    }
    building_geometry.new_coords = new_coords;


    // Scale the CityObject holes in the scene
    for (auto& list_hole : building_geometry.holes){
      std::vector<std::array<double,2>> new_hole_l = {};
      for (auto& hole : list_hole){
        double x = (double) hole[0];
        double y = (double) hole[1];

        double new_hole_x = (x - class_coord.x_minimum) / (class_coord.x_maximum - class_coord.x_minimum) * scale - (
                                scale / 2);
        double new_hole_y = (y - class_coord.y_minimum) / (class_coord.y_maximum - class_coord.y_minimum) * scale - (
                                scale / 2);
        std::array<double,2> new_hole_coords = {new_hole_x, new_hole_y};
        new_hole_l.push_back(new_hole_coords);
      }
      new_holes.push_back(new_hole_l);
    }
    building_geometry.new_holes = new_holes;
  
    //std::cout << "------" << std::endl;
    //std::cout << building_geometry["new_holes"] << std::endl;
    all_objects.push_back(building_geometry);

  }
  //std::cout << all_objects << std::endl;
  return all_objects;        
}




//  ---------------- MAIN FUNCTION --------------------------

int main(int argc, const char* argv[]) {
  // application
  auto app_guard = std::make_unique<app_state>();
  auto app       = app_guard.get();

  // command line options
  auto camera_name = ""s;
  auto add_skyenv  = false;

  // parse command line
  auto cli = cli::make_cli("save_city", "save the scene");
  add_option(cli, "--camera", camera_name, "Camera name.");
  add_option(cli, "--geojson,-g", app->geojson_filename, "Geojson filename", true);
  add_option(cli, "--save,-fs", app->filename_save, "Save filename", true);

  parse_cli(cli, argc, argv);


  //------------ PREPARE DATA TO LOAD THE SCENE ----------------

  // Path to the GeoJson file
  const std::string path = app->geojson_filename; // "./geojson/";

  // Read GeoJson files
  nlohmann::json geojson_file;
  std::vector<CityObject> all_buildings = {};
  auto class_coord = Coordinate();

  for (const auto &file : sfs::directory_iterator(path)){
    //std::cout << file.path() << std::endl;
    if(file.path().extension() == ".geojson"){
      std::ifstream filename(file.path());
      geojson_file = json::parse(filename);
      all_buildings = generate_new_coordinates(geojson_file, all_buildings, class_coord);
    }
  }
      
  app->all_geometries = all_buildings;

  // Create city
  auto error       = ""s;
  /*clock_t start,end;
  double tempo;
  start=clock();*/

  if (!create_city_from_json(app->ioscene, app->all_geometries)){ 
      std::cout << " City not created! " << std::endl;
  }

  /*end=clock();
  tempo=((double)(end-start))/CLOCKS_PER_SEC;  // time in seconds

  std::cout << tempo << std::endl;*/
  
  // Save the scene
  std::string save_path = app->filename_save; //"./scene/city.json";
  sio::save_scene(save_path, app->ioscene, error);
  save_path = ""s;

  // Done
  return 0;
}






