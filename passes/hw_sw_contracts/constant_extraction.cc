#include "kernel/register.h"
#include "kernel/ffinit.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/celltypes.h"
#include "kernel/json.h"
#include "libs/sha1/sha1.h"
#include "passes/hw_sw_contracts/configuration_file.cc"
#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>
#include <queue>


USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN


struct ConstantExtraction : public Pass {
    ConstantExtraction() : Pass("constant_extraction", "Extract constants from the module") { }
	void help() override
	{
		log("\n");
		log("    constant_extraction <output_file> \n");
		log("\n");
	}

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing ANALYZE_DEPENDENCIES pass.\n");

        if(args.size() != 2){
            assert(false);
        }

        if(design->selected_modules().size() != 1){
			log_error("More that one module selected\n");
		}
        Module *mod = design->selected_modules()[0];

        std::string output_file = args[1];

        std::set<string> answer;

        for(Cell *cell : mod->cells()){
            for(auto [portname, sigSpec] : cell->connections()){
                if(sigSpec.is_fully_const()){
                    assert(!sigSpec.empty());
                    answer.insert(sigSpec.as_const().as_string());
                }
            }
        }

        vector<string> answer_vec;
        for(auto c : answer){
            answer_vec.push_back(c);
        }
        export_constants_to_file(output_file, answer_vec);
    }

    void export_constants_to_file(string output_file, vector<string> constants){
        std::ofstream out(output_file);

        for(auto c : constants){
            out << c << "\n";
        }
    }


} AnalyzeDependencies;

PRIVATE_NAMESPACE_END
