#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "libs/json11/json11.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ConfigurationFile {
    std::string clock_name;

    ConfigurationFile() { }

    ConfigurationFile(std::string filename){
        std::ifstream input(filename);
        
        // check if file exists
        if(!input.good()){
            log_error("ERROR: invalid configuration file");
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string contents = buffer.str();

        std::string err = "";

        json11::Json json = Json::parse(contents, err);

        if(err != ""){
            log_error("ERROR: failure when reading a json file");
        }

        if(!json.is_object()){
            log_error("ERROR: expected object in configuration file");
        }

        auto json_items = json.object_items();

        if(json_items.find("clock_name") == json_items.end()){
            log_error("ERROR: configuration files does not contain a name for clock");
        }
        if(!json_items["clock_name"].is_string()){
            log_error("ERROR: name for a clock must be of type string");
        }
        clock_name = json_items["clock_name"].string_value();
    }
};

PRIVATE_NAMESPACE_END
