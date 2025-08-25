/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

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

bool stringToInt(const std::string &s, int &result) {
    try {
        size_t pos;
        result = std::stoi(s, &pos);

        if (pos != s.size()) {
            return false;
        }
        return true;
    } 
    catch (const std::invalid_argument &) {
        return false;
    } 
    catch (const std::out_of_range &) {
        return false;
    }
}


struct DumpListOfWires : public Pass {
	DumpListOfWires() : Pass("dump_list_of_wires", "Dumps all wires into a file") { }
	void help() override
	{
		log("\n");
		log("    dump_list_of_wires output_file_name \n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing DUMP_LIST_OF_WIRES pass.\n");

		if(args.size() != ((size_t) 2)){
			log_error("FAILURE: incorrect arguments for dump_list_of_wires");
			return;
		}

		std::string out_filename = args[1];

		if(design->selected_modules().size() > 1){
			log_warning("WARNING: more that one module selected");
		}
        Module *mod = design->selected_modules()[0];

		vector<std::string> wire_names;
		for(Wire *wire : mod->wires()){
			wire_names.push_back(RTLIL::unescape_id(wire->name.str()));
		}
		
		std::ofstream output(out_filename);
		output << wire_names.size() << "\n";
		for(auto name : wire_names){
			output << name << "\n";
		}
	}


} DumpListOfWires;

PRIVATE_NAMESPACE_END
