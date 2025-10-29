#include "kernel/consteval.h"
#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "passes/hw_sw_contracts/simplification_parameters.cc"
#include "passes/hw_sw_contracts/generatable_expressions/basic_expressions.cc"
#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>
#include <random>
#include <limits>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct RunResult{
    unsigned int applicability, observation;
    RunResult(unsigned int applicability, unsigned int observation) : applicability(applicability), observation(observation) { }
};

vector<unsigned int> generate_random_values(vector<Wire*> wires, std::mt19937 &gen){
    std::uniform_int_distribution<unsigned int> dist(std::numeric_limits<unsigned int>::min(), std::numeric_limits<unsigned int>::max());
    
    vector<unsigned int> ans;
    for(Wire *wire : wires){
        unsigned int random_value = dist(gen);
        assert(wire->width <= 32);
        if(wire->width < 32){
            random_value = ((1U<<wire->width) - 1) & random_value;
        }
        ans.push_back(random_value);
   }
   return ans;
}

void set_values_randomly(std::map<Wire*, unsigned int> &values, vector<Wire*> wires, std::mt19937 &gen){
    vector<unsigned int> vals = generate_random_values(wires, gen);
    for(size_t id = 0; id < wires.size(); id++){
        Wire *wire = wires[id];
        values[wire] = vals[id];
    }
}

RunResult run_module(Module *mod, std::map<Wire*, unsigned int> values, SimplificationParameters &param){
    ConstEval ce(mod);
    for(auto [wire, val] : values){
        vector<bool> val_bits;
        for(int i = 0; i < wire->width; i++){
            val_bits.push_back(((1<<i) & val) > 0);
        }
        RTLIL::Const const_val(val_bits);
        // std::cout << "GetSize(const_val) = " << GetSize(const_val) << std::endl;
        // std::cout << "GetSize(wire) = " << GetSize(wire) << std::endl;
        // std::cout << "val = " << val << std::endl;
        // std::cout << "const_val = " << const_val.as_int() << std::endl;
        ce.set(wire, const_val);  // IN1 = 1 (1-bit)
    }

    SigSpec app_sig = mod->wire(RTLIL::escape_id(param.applicability)); 
    SigSpec obs_sig = mod->wire(RTLIL::escape_id(param.observation));
    SigSpec app_pending, obs_pending;  
    bool success = ce.eval(app_sig, app_pending);
    success = success && ce.eval(obs_sig, obs_pending);
    if (success) {
        // out_sig is now replaced with the computed constant value(s)
        return RunResult(app_sig.as_int(), obs_sig.as_int());
    } else {
        // Evaluation failed – some required signals were not set
        log_error("Cannot evaluate outputs, need value for %s %s \n", log_signal(app_pending), log_signal(obs_pending));
    }
}

struct ContractPredictorSimplification : public Pass {
    SimplificationParameters param;

    ContractPredictorSimplification() : Pass("simplify_predictor", "Simplifies the contract") { }
	void help() override
	{
		log("\n");
		log("    simplify_predictor <main_module> <predictor_module> <simplification_parameter_file>\n");
		log("\n");
	}


	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
        const unsigned int rng_seed = 123456789;
        std::mt19937 gen(rng_seed);
		log_header(design, "Executing SIMPLIFY_PREDICTOR pass.\n");

        if(args.size() != 4){
			log_error("Incorrect number of arguments\n");
		}

        std::string main_module_name = args[1];
        IdString main_module_id = RTLIL::escape_id(main_module_name);
        Module *main_mod = design->module(main_module_id);
        assert(main_mod != nullptr);

        std::string predictor_module_name = args[2];
        IdString predictor_module_id = RTLIL::escape_id(predictor_module_name);
        Module *predictor_mod = design->module(predictor_module_id);
        assert(predictor_mod != nullptr);
        
        std::string parameter_file = args[3];
        param.parse_parameters(parameter_file);
        param.check_validity(predictor_mod);

        vector<Wire*> control_inputs = param.extract_control_wires(predictor_mod);

        std::set<Wire*> control_input_set(control_inputs.begin(), control_inputs.end());

        vector<Wire*> data_inputs;
        for(Wire *wire : predictor_mod->wires()){
            if((wire->port_input) && (control_input_set.find(wire) == control_input_set.end())){
                data_inputs.push_back(wire);
            }
        }

        vector<int> control_input_widths;
        for(Wire *wire : control_inputs){
            control_input_widths.push_back(wire->width);
        }

        vector<unsigned int> control_values = initial_control_values(control_input_widths);

        vector<vector<unsigned int>> expression_conditions;
        vector<BasicExpression*> expressions;
        do{
            control_values = next_control_values(control_values, control_input_widths);
            std::cout << "control values are: " << std::endl;
            for(auto ctrl : control_values){
                std::cout << ctrl << ", ";
            }
            std::cout << std::endl;

            std::map<Wire*, unsigned int> values;
            
            for(size_t i = 0; i < control_inputs.size(); i++){
                Wire *wire = control_inputs[i];
                unsigned int val = control_values[i];
                values[wire] = val;
            }

            set_values_randomly(values, data_inputs, gen);

            RunResult applicability_evaluation = run_module(predictor_mod, values, param);
            if(!applicability_evaluation.applicability){
                continue;
            }

            vector<vector<unsigned int> > test_inputs;
            vector<RunResult> test_results;
            for(int i = 0; i < param.test_cnt; i++){
                auto vals = generate_random_values(data_inputs, gen);
                test_inputs.push_back(vals);
                for(size_t wire_id = 0; wire_id < data_inputs.size(); wire_id++){
                    values[data_inputs[wire_id]] = vals[wire_id];
                }
                test_results.push_back(run_module(predictor_mod, values, param));
            }

            bool found = false;
            for(int sz = 1; sz <= param.expression_complexity; sz++){
                vector<BasicExpression*> basic_expressions = generate_expressions(sz, data_inputs.size());
                for(auto exp : basic_expressions){
                    bool valid = true;
                    for(size_t input_id = 0; input_id < test_inputs.size(); input_id++){
                        auto inp = test_inputs[input_id];
                        auto res = test_results[input_id];
                        if(res.observation != exp->evaluate(inp)){
                            valid = false;
                            break;
                        }
                    }
                    if(valid){
                        found = true;
                        expressions.push_back(exp);
                        expression_conditions.push_back(control_values);
                        break;
                    }
                }
                if(found){
                    break;
                }
            }
            assert(found);

        } while(control_values != initial_control_values(control_input_widths));

        Module *simplified_module = make_simplified_module(design, predictor_mod, control_inputs, data_inputs, expression_conditions, expressions);
        add_predictor_to_mod(main_mod, simplified_module, RTLIL::escape_id(param.observation), RTLIL::escape_id("main_" + param.observation), RTLIL::escape_id(param.applicability), RTLIL::escape_id("main_" +param.applicability));
	}

    vector<unsigned int> initial_control_values(vector<int> widths){
        return vector<unsigned int>(widths.size(), 0);
    }

    vector<unsigned int> next_control_values(vector<unsigned int> values, vector<int> widths){
        reverse(values.begin(), values.end());
        reverse(widths.begin(), widths.end());
        vector<unsigned int> next_values = values;

        int carry = 1;
        for(size_t i = 0; i < widths.size(); i++){
            unsigned int next_carry = (next_values[i] > 0) ? 1 : 0;
            
            next_values[i] += carry;
            if(next_values[i] == (1u<<widths[i])){
                next_values[i] = 0;
            }

            if(next_values[i] != 0){
                next_carry = 0;
            }
            carry = next_carry;
        }
        reverse(next_values.begin(), next_values.end());
        return next_values;
    }

    Wire* compare_wire_to_constant(Module* mod, Wire *wire, unsigned int value){
        Cell *andCell = mod->addCell(mod->uniquify(RTLIL::escape_id("and_cell")), ID($eq));

        andCell->setPort(ID::A, wire);
        andCell->setPort(ID::B, value);

        andCell->setParam(ID::A_SIGNED, false);
        andCell->setParam(ID::B_SIGNED, false);
        andCell->setParam(ID::Y_WIDTH, 1);
        andCell->setParam(ID::A_WIDTH, wire->width);
        // TODO: the following is probably wrong in some weird way that is also very difficult to debug way
        andCell->setParam(ID::B_WIDTH, 32);

        Wire *outWire = mod->addWire(mod->uniquify(RTLIL::escape_id("comparison_output")), 1);
        andCell->setPort(ID::Y, outWire);

        return outWire;
    }

    SigSpec take_and_of_wires(Module* mod, vector<Wire*> wires){
        for(Wire *wire : wires){
            assert(wire->width == 1);
        }

        SigSpec all_true = SigSpec(true);

        SigSpec cur = all_true;

        for(Wire *wire : wires){
            assert(wire->width == 1);
            Cell *andCell = mod->addCell(mod->uniquify(RTLIL::escape_id("and_cell")), ID($_AND_));

            andCell->setPort(ID::A, cur);
            andCell->setPort(ID::B, wire);

            Wire *outWire = mod->addWire(mod->uniquify(RTLIL::escape_id("and_out")), 1);
            andCell->setPort(ID::Y, outWire);

            cur = outWire;
        }

        return cur;

    }

    // this function assumes that expression conditions are disjoint
    Module* make_simplified_module(Design *design, Module *predictor_mod, vector<Wire*> mod_control_inputs, vector<Wire*> mod_data_inputs, vector<vector<unsigned int>> expression_conditions, vector<BasicExpression*> expressions){
        Module *simplified_module = design->addModule(RTLIL::escape_id(param.simplified_module_name));
        for(Wire *wire : predictor_mod->wires()){
            if((wire->port_input) || (wire->port_output)) {
                simplified_module->addWire(wire->name, wire);
            }
        }

        vector<Wire*> control_inputs;
        for(Wire *wire : mod_control_inputs){
            control_inputs.push_back(simplified_module->wire(wire->name));
        }

        vector<Wire*> data_inputs;
        for(Wire *wire : mod_data_inputs){
            data_inputs.push_back(simplified_module->wire(wire->name));
        }

        vector<SigSpec> expression_applicability;
        for(size_t id = 0; id < expressions.size(); id++){
            vector<unsigned int> conds = expression_conditions[id];
            vector<Wire*> condition_parts;
            for(size_t i = 0; i < conds.size(); i++){
                condition_parts.push_back(compare_wire_to_constant(simplified_module, control_inputs[i], conds[i]));
            }
            expression_applicability.push_back(take_and_of_wires(simplified_module, condition_parts));
        }

        std::cout << "viso expressionu yra: " << expressions.size() << std::endl;
        for(auto expression : expressions){
            std::cout << "expression: " << expression->to_string(data_inputs) << std::endl;
        }

        vector<SigSpec> applicability_results(expression_applicability.size(), SigSpec(false));
        create_simplification_pmux(simplified_module, RTLIL::escape_id(param.applicability), SigSpec(false), expression_applicability, applicability_results, "applicability");


        // TODO: I think this will fail as data variables can have different sizes and then the final result can be of different size
        vector<SigSpec> expression_sigspecs;
        for(BasicExpression *exp : expressions){
            expression_sigspecs.push_back(exp->convert_to_rtlil(simplified_module, data_inputs));
        }

        create_simplification_pmux(simplified_module, RTLIL::escape_id(param.observation), 0, expression_applicability, expression_sigspecs, "observation");

        simplified_module->fixup_ports();
        return simplified_module;
    }

    // specific_name - some word that is used in naming cells
    void create_simplification_pmux(Module *simplified_module, IdString result_name, SigSpec default_value, vector<SigSpec> applicabilities, vector<SigSpec> expressions, std::string specific_name){
        Wire *result_wire = simplified_module->wire(result_name);
        assert(result_wire != nullptr);

        Cell *pmux = simplified_module->addCell(simplified_module->uniquify(RTLIL::escape_id(specific_name + "_pmux")), ID($pmux));

        assert(GetSize(default_value) == GetSize(result_wire));
        pmux->setPort(ID::A, default_value);

        pmux->setParam(ID::WIDTH, result_wire->width);
        SigSpec concatenated_selection;
        for(SigSpec sig_spec : expressions){
            assert(GetSize(sig_spec) == GetSize(result_wire));
            concatenated_selection.append(sig_spec);
        }
        pmux->setPort(ID::B, concatenated_selection);

        SigSpec concatenated_applicabilities = SigSpec();
        for(SigSpec sig_spec: applicabilities){
            assert(GetSize(sig_spec) == 1);
            concatenated_applicabilities.append(sig_spec);
        }
        pmux->setParam(ID::S_WIDTH, applicabilities.size());
        pmux->setPort(ID::S, concatenated_applicabilities);

        pmux->setPort(ID::Y, result_wire);
    }

} ContractPredictorSimplification;

PRIVATE_NAMESPACE_END
