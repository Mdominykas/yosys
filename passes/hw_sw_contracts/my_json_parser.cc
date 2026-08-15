#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct MyJsonParser{
    Json::array parse_array_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_array()){
            const std::string error_msg = "Value of " + name + " must be of an array type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].array_items();
    }

    std::string cast_to_string(Json::object obj, std::string name){
        if(!obj[name].is_string()){
            const std::string error_msg = "Value of  " + name + " must be of a string type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].string_value();
    }


    std::string parse_string_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        return cast_to_string(obj, name);
    }

    int parse_int_from_object(Json::object obj, std::string name){
        if(obj.find(name) == obj.end()){
            const std::string error_msg = "Value for the " + name + " not found\n";
            log_error("%s", error_msg.c_str());
        }
        if(!obj[name].is_number()){
            const std::string error_msg = "Value of  " + name + " must be of a int type\n";
            log_error("%s", error_msg.c_str());
        }

        return obj[name].int_value();
    }

};

PRIVATE_NAMESPACE_END
