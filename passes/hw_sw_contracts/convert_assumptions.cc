#include "kernel/register.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "passes/hw_sw_contracts/lifting_wires.h"
#include "passes/hw_sw_contracts/utils.h"
#include "passes/hw_sw_contracts/my_json_parser.cc"


#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PreprocessedAssumption{
    std::string leave_name;
    std::vector<std::string> variables;
    std::string cond;
    PreprocessedAssumption(std::string leave_name, std::vector<std::string> variables, std::string cond) : leave_name(leave_name), variables(variables), cond(cond){

    }
};

struct PreprocessedAssumptionConfig : public MyJsonParser{
    vector<PreprocessedAssumption> preprocessed_assumptions;
    void parse_input_from_file(string filename){
        assert(preprocessed_assumptions.empty());
        std::ifstream input(filename);

        if(!input.good()){
            log_error("Invalid configuration file\n");
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        std::string contents = buffer.str();

        std::string err = "";

        json11::Json assumption_json = Json::parse(contents, err);

        if(err != ""){
            std::cout << "contents are: " << contents << std::endl;
            log_error("Failure when reading a assumption_json file\nThe error is: %s\n", err.c_str());
        }

        if(!assumption_json.is_array()){
            log_error("Error, json in the assumption file is not an array\n");
        }

        auto items = assumption_json.array_items();
        for(auto item : items){
            assert(item.is_object());
            std::string leave_name = parse_string_from_object(item.object_items(), "leave_name");
            
            auto var_list = parse_array_from_object(item.object_items(), "variables");
            vector<std::string> variables;
            for(Json var : var_list){
                assert(var.is_string());
                variables.push_back(var.string_value());
            }

            std::string cond = parse_string_from_object(item.object_items(), "cond");

            preprocessed_assumptions.push_back(PreprocessedAssumption(leave_name, variables, cond));
        }
    }
};


struct AdditionalDeclaration {
    std::string module_name;
    std::string wire_name;
    int width;
    AdditionalDeclaration(std::string module_name, std::string wire_name, int width) : module_name(module_name), wire_name(wire_name), width(width) { }

    bool operator==(const AdditionalDeclaration &other) const {
    return module_name == other.module_name &&
           wire_name == other.wire_name &&
           width == other.width;
    }
};


struct ConvertAssumptions : public Pass {
    struct ModuleTree {
        struct ModuleEdge;
        struct ModuleNode;
        struct ModuleNode {
            std::string module_name;
            ModuleNode *parent;
            int depth;
            vector<ModuleEdge> edges;
            ModuleNode(std::string module_name, int depth, ModuleNode *parent) : module_name(module_name), parent(parent), depth(depth) { }
        };
        struct ModuleEdge {
            std::string instance_name;
            ModuleNode *type;
            ModuleEdge(std::string instance_name, ModuleNode *type) : instance_name(instance_name), type(type) { }
        };

        std::map<ModuleNode*, RTLIL::Module*> to_rtlil_module;
        ModuleNode *root = nullptr;
        ModuleTree(std::string top_module, RTLIL::Module *top){
            root = add_node(top_module, top);
        }

        std::map<std::string, ModuleNode*> module_map;
        ModuleNode* add_node(std::string module_name, RTLIL::Module *mod){
            assert(module_map.find(module_name) == module_map.end());
            ModuleNode *node = new ModuleNode(module_name, 0, nullptr);
            module_map[module_name] = node;
            to_rtlil_module[node] = mod;
            return node;
        }

        ModuleNode* get_or_add_node(std::string module_name, ModuleNode *parent, RTLIL::Module *mod){
            if(module_map.find(module_name) != module_map.end()){
                return module_map[module_name];
            }
            ModuleNode *node = new ModuleNode(module_name, parent->depth + 1, parent);
            module_map[module_name] = node;
            to_rtlil_module[node] = mod;
            return node;
        }

        ModuleNode* go_deeper(ModuleNode *cur, std::string module_name, std::string instance_name, RTLIL::Module *mod){
            for(ModuleEdge edge : cur->edges){
                if(edge.instance_name == instance_name){
                    assert(edge.type->module_name == module_name);
                    return edge.type;
                }
            }
            ModuleNode *node = get_or_add_node(module_name, cur, mod);
            cur->edges.push_back(ModuleEdge(instance_name, node));
            return node;
        }

        ~ModuleTree() {
            for (auto &it : module_map)
                delete it.second;
        }

        // The default copy would duplicate the pointers and delete every node twice.
        ModuleTree(const ModuleTree &) = delete;
        ModuleTree &operator=(const ModuleTree &) = delete;
    };

    ConvertAssumptions() : Pass("convert_assumptions", "Takes assumption json file as an and outputs the assumptions in different format + the required wire_liftings") { }

    void help() override
	{
		log("\n");
		log("    convert_assumptions <configuration_file> <assumption_output_file> <wire_lifting_outputs> \n");
		log("\n");
	}

    // Module name with parameterization stripped, so "$paramod\fifo\W=8" -> "fifo".
    static std::string base_name(RTLIL::Module *mod)
    {
        if (mod->name.begins_with("$paramod")) {
            if (!mod->has_attribute(ID::hdlname))
                log_error("Module %s has no hdlname attribute.\n", mod);
            return mod->get_string_attribute(ID::hdlname);
        }
        return RTLIL::unescape_id(mod->name);
    }

    // fails if the desgin contains some module that an assumption is added more than once
    void check_single_module_hypothesis(RTLIL::Design *design, ModuleTree &tree){
        std::map<std::string, int> cell_count;
        for (auto mod : design->modules()){
            for (auto cell : mod->cells()){
                if (RTLIL::Module *child = design->module(cell->type)){
                    cell_count[base_name(child)]++;
                }
            }
        }

        for (auto &it : tree.module_map){
            if ((cell_count[it.first] != 1) && (it.second != tree.root)){
                log_error("Module %s is instantiated %d times.\n",
                        it.first.c_str(), cell_count[it.first]);
            }
        }
    }

    ModuleTree::ModuleNode* find_lowest_common_ancestor(vector<ModuleTree::ModuleNode*> last_modules){
        assert(last_modules.size() > 0);
        int first = 0;
        for(size_t cur = 1; cur < last_modules.size(); cur++){
            while(last_modules[cur]->depth > last_modules[first]->depth){
                last_modules[cur] = last_modules[cur]->parent;
            }
            while(last_modules[cur]->depth < last_modules[first]->depth){
                last_modules[first] = last_modules[first]->parent;
            }
            while(last_modules[cur] != last_modules[first]){
                last_modules[cur] = last_modules[cur]->parent;
                last_modules[first] = last_modules[first]->parent;
            }
        }
        return last_modules[first];
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
        log_header(design, "Executing CONVERT ASSUMPTIONS pass.\n");

        if(args.size() != 4){
			log_error("Incorrect number of arguments\n");
		}

        std::string configuration_file_name = args[1];
        std::string assumption_output_file = args[2];
        std::string wire_lifting_output_file = args[3];

        vector<std::pair<std::string, vector<json11::Json> > > module_assumptions;

        auto config = PreprocessedAssumptionConfig();
        config.parse_input_from_file(configuration_file_name);

        RTLIL::Module *top = design->top_module();
        ModuleTree tree = ModuleTree(RTLIL::unescape_id(top->name.str()), top);

        std::vector<SingleWireLifting> wire_liftings;
        std::vector<AdditionalDeclaration> additional_declarations;
        for(auto a : config.preprocessed_assumptions){
            vector<ModuleTree::ModuleNode*> last_modules;
            vector<bool> needs_escaping;
            vector<int> widths;
            for(auto var : a.variables){
                vector<string> parts = split_into_parts(var);
                ModuleTree::ModuleNode *cur_node = tree.root;
                RTLIL::Module *cur_module = top;
                for(int i = 0; i + 1 < ((int) parts.size()); i++){
                    std::string instance_name = parts[i];
                    RTLIL::Cell *cell = cur_module->cell(RTLIL::escape_id(instance_name));
                    assert(cell != nullptr);
                    // after 'hierarchy -auto-top' pass, there are only parametrized modules
                    RTLIL::Module *pos_mod = design->module(cell->type);
                    std::string child_module = base_name(pos_mod);

                    cur_node = tree.go_deeper(cur_node, child_module, instance_name, pos_mod);
                    cur_module = pos_mod;
                }
                last_modules.push_back(cur_node);
                needs_escaping.push_back((parts.size()) > 1);
                std::string last_wire_name = RTLIL::escape_id(parts.back());
                if(cur_module->wire(last_wire_name) == nullptr){
                    log_error("Wire named %s is not in module %s ", last_wire_name, cur_module->name);
                }
                widths.push_back(cur_module->wire(last_wire_name)->width);
            }
            ModuleTree::ModuleNode* common_ancestor = find_lowest_common_ancestor(last_modules);
            assert(common_ancestor != nullptr);


            vector<string> last_names;
            for(size_t i = 0; i < a.variables.size(); i++){
                vector<string> parts = split_into_parts(a.variables[i]);
                int last_depth = last_modules[i]->depth;
                int common_depth = common_ancestor-> depth;

                std::string concat_suffix = parts[last_depth];
                ModuleTree::ModuleNode* cur_mod = last_modules[i];

                for(int d = last_depth; d > common_depth; d--){
                    std::string higher = parts[d - 1] + "___" + concat_suffix;


                    SingleWireLifting wire_lifting = SingleWireLifting(cur_mod->module_name, cur_mod->parent->module_name, parts[d - 1], concat_suffix, higher);

                    bool need_to_add_lifting = true;
                    for(SingleWireLifting wl : wire_liftings){
                        if(wl == wire_lifting){
                            need_to_add_lifting = false;
                            break;
                        }
                    }
                    if(need_to_add_lifting){
                        wire_liftings.push_back(wire_lifting);
                    }

                    int width = widths[i];
                    AdditionalDeclaration new_decl = AdditionalDeclaration(cur_mod->parent->module_name, higher, width);
                    
                    bool needs_additional_declaration = true;
                    for(AdditionalDeclaration add_decl : additional_declarations){
                        if(add_decl == new_decl){
                            needs_additional_declaration = false;
                            break;
                        }
                    }

                    if(needs_additional_declaration){
                        additional_declarations.push_back(new_decl);

                    }

                    concat_suffix = higher;
                    cur_mod = cur_mod->parent;
                }

                last_names.push_back(concat_suffix);
            }

            std::string modified_cond = a.cond;
            std::string module_to_place = common_ancestor->module_name;
            for(size_t i = 0; i < a.variables.size(); i++){
                string var_name = a.variables[i];
                if(needs_escaping[i]){
                    var_name = "\\" + var_name + " ";
                }
                int replaced_cnt = replaceAll(modified_cond, var_name, last_names[i]);
                if(replaced_cnt == 0){
                    log_error("Tried to replace \"%s\" with \"%s\" inside \"%s\"\n", var_name.c_str(), last_names[i].c_str(), modified_cond.c_str());
                }
                assert(replaced_cnt > 0);
            }
            auto statement_obj = json11::Json(json11::Json::object {
                { "verilog_statement", modified_cond },
                { "leave_name", a.leave_name }
            });

            // TODO: this functionality can be extracted into a function
            bool placed = false;
            for(size_t i = 0; i < module_assumptions.size(); i++){
                std::string mod_name = module_assumptions[i].first;
                if(mod_name == module_to_place){
                    module_assumptions[i].second.push_back(statement_obj);
                    placed = true;
                    break;
                }
            }

            if(!placed){
                module_assumptions.push_back({module_to_place, {statement_obj}});
            }
        }

        check_single_module_hypothesis(design, tree);

        PrettyJson assumption_json;
        if (!assumption_json.write_to_file(assumption_output_file)){
            log_error("Cannot open %s for writing\n", assumption_output_file.c_str());
        }
        
        assumption_json.begin_object();

        assumption_json.name("wire_declarations");
        assumption_json.begin_array();
        for(auto decl : additional_declarations){
            auto decl_object = json11::Json(json11::Json::object {
                {"module", decl.module_name + ".v"},
                {"name", decl.wire_name},
                {"width", decl.width}
            });
            assumption_json.value(decl_object);
        }
        assumption_json.end_array();

        assumption_json.name("assumptions");
        assumption_json.begin_array();
        for(size_t i = 0; i < module_assumptions.size(); i++){
            assumption_json.begin_object();
            assumption_json.entry("module", module_assumptions[i].first + ".v");
            assumption_json.name("statements");
            assumption_json.begin_array();
            for(auto st : module_assumptions[i].second){
                assumption_json.value(st);
            }
            assumption_json.end_array();
            assumption_json.end_object();
        }
        assumption_json.end_array();

        assumption_json.end_object();

        PrettyJson wire_lifting_json;
        if (!wire_lifting_json.write_to_file(wire_lifting_output_file)){
            log_error("Cannot open %s for writing\n", wire_lifting_output_file.c_str());
        }

        wire_lifting_json.begin_object();

        wire_lifting_json.name("liftings");
        wire_lifting_json.begin_array();
        for(SingleWireLifting lifting: wire_liftings){
            auto lifting_object = json11::Json(json11::Json::object {
                {"inner_module", lifting.inner_module},
                {"outer_module", lifting.outer_module},
                {"instance_name", lifting.instance_name},
                {"inner_wire", lifting.inner_wire},
                {"outer_wire", lifting.outer_wire}
            });
            wire_lifting_json.value(lifting_object);
        }
        wire_lifting_json.end_array();

        wire_lifting_json.end_object();
    }

} ConvertAssumptions;


PRIVATE_NAMESPACE_END
