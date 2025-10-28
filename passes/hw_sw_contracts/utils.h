#ifndef HARDWARE_SOFTWARE_CONTRACT_UTILS
#define HARDWARE_SOFTWARE_CONTRACT_UTILS

#include <string>

YOSYS_NAMESPACE_BEGIN
namespace hardware_software_contracts {

    IdString remove_input_from_wire_name(IdString wire_name);
    IdString add_input_to_wire_name(IdString wire_name);
    void add_predictor_to_mod(Module *mod, Module *predictor, IdString observation_in_predictor, IdString observation_in_mod, IdString applicability_in_predictor, IdString applicability_in_mod);
    void make_wire_input(Module *predictor, Wire *wire, IdString new_wire_name);

}; /* namespace hardware_software_contracts */
YOSYS_NAMESPACE_END

#endif /* HARDWARE_SOFTWARE_CONTRACT_UTILS */
