// reset_state.cc - Yosys pass: write the value every register has after reset, as the "state:" section
// of the LeaVe configuration (ready to paste).
//
//   build:  yosys-config --build reset_state.so reset_state.cc
//   use:    yosys -m reset_state.so ...   hierarchy -top cv32e40s_wrapper; proc; flatten;
//                                         get_reset_state -o reset_state.yaml cv32e40s_wrapper
//
// One entry per register (a wire whose bits are all flip-flop outputs). The value after reset is the async
// reset value (read_slang: async load of a constant), else the sync reset value, else the init value.
// Registers without a known reset value get no "val" (symbolic). Run it right after proc and flatten.

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/ff.h"
#include <fstream>
#if __has_include("kernel/newcelltypes.h")
#include "kernel/newcelltypes.h"
#define IS_FF(type) StaticCellTypes::categories.is_ff(type)
#else
#define IS_FF(type) (RTLIL::builtin_ff_cell_types().count(type) > 0)
#endif

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// decimal value of bits (bits[0] is the LSB); empty string if a bit is not 0 or 1
static std::string to_decimal(const std::vector<State> &bits)
{
	std::vector<int> digits{0};                      // little-endian decimal digits
	for (int i = GetSize(bits) - 1; i >= 0; i--) {
		if (bits[i] != State::S0 && bits[i] != State::S1) {
			return "";
		}
		int carry = bits[i] == State::S1;
		for (auto &d : digits) {
			int x = d * 2 + carry;
			d = x % 10;
			carry = x / 10;
		}
		if (carry) {
			digits.push_back(carry);
		}
	}
	std::string s;
	for (auto it = digits.rbegin(); it != digits.rend(); ++it)
		s += char('0' + *it);
	return s;
}

struct GetResetStatePass : public Pass {
	GetResetStatePass() : Pass("get_reset_state", "write the value of every register after reset (LeaVe state section)") { }
	void help() override
	{
		log("\n");
		log("    get_reset_state [-o <file>] [<module>]\n");
		log("\n");
		log("Writes the \"state:\" section of the LeaVe configuration with the value every register of\n");
		log("<module> (default: the top module) has after reset. Run it after proc and flatten.\n");
		log("Default output file: reset_state.yaml\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing GET_RESET_STATE pass.\n");
		std::string out_file = "reset_state.yaml";
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-o" && argidx + 1 < args.size()) {
				out_file = args[++argidx];
				continue;
			}
			break;
		}
		RTLIL::Module *module = argidx < args.size() ? design->module(RTLIL::escape_id(args[argidx])) : design->top_module();
		if (module == nullptr) {
			log_cmd_error("Module not found (give the top module name).\n");
		}

		// value after reset of every flip-flop output bit, per wire (Sm: bit is not a flip-flop)
		SigMap sigmap(module);
		std::map<std::string, std::vector<State>> regs;
		int n_reset = 0;
		for (auto cell : module->cells()) {
			if (!IS_FF(cell->type)) {
				continue;
			}
			FfData ff(nullptr, cell);
			Const value = ff.val_init;
			if (ff.has_arst) {
				value = ff.val_arst;
			}
			else if (ff.has_srst) {
				value = ff.val_srst;
			}
			else if (ff.has_aload && sigmap(ff.sig_ad).is_fully_const()) {   // read_slang: async load of the reset value
				value = sigmap(ff.sig_ad).as_const();
			}
			if (ff.has_arst || ff.has_srst || ff.has_aload) {
				n_reset++;
			}
			for (int i = 0; i < GetSize(ff.sig_q); i++) {
				SigBit q = ff.sig_q[i];
				// skip internal wires and function/task temporaries (names with '$')
				if (q.wire == nullptr || !q.wire->name.isPublic() || q.wire->name.str().find('$') != std::string::npos) {
					continue;
				}
				std::vector<State> &bits = regs[RTLIL::unescape_id(q.wire->name)];
				if (bits.empty()) {
					bits.assign(q.wire->width, State::Sm);
				}
				bits[q.offset] = i < GetSize(value) ? value[i] : State::Sx;
			}
		}

		// one entry per wire that is entirely flip-flops (wires that are only partly flip-flops are left out)
		std::ofstream f(out_file);
		f << "state:\n";
		int count = 0;
		for (auto &it : regs) {
			const std::vector<State> &bits = it.second;
			if (std::find(bits.begin(), bits.end(), State::Sm) != bits.end()) {
				continue;
			}
			std::string val = to_decimal(bits);
			f << "  - {id: \"\\\\" << it.first << "\", expr: \"\\\\" << it.first << "\", width: " << GetSize(bits);
			if (!val.empty()) {
				f << " , val : " << val;
			}
			f << "}\n";
			count++;
		}
		if (n_reset == 0) {
			log_warning("No flip-flop has a reset. Run get_reset_state right after proc (and flatten), before anything that\n"
			            "removes or rewrites the reset (tying rst_ni to a constant followed by opt, async2sync, clk2fflogic).\n");
		}
		log("Wrote %d registers to %s.\n", count, out_file.c_str());
	}
} GetResetStatePass;

PRIVATE_NAMESPACE_END