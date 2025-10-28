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

Wire *extend_wire(Module *mod, Wire *wire, int target_width, bool is_signed){
    assert(!is_signed); // I do not support wires being signed
    assert(wire->width <= target_width);
    if(wire->width == target_width){
        return wire;
    }
    Wire *extended_wire = mod->addWire(wire->name.str() + "_extended", target_width);
    
    int sign_bit_cnt = target_width - wire->width;

    vector<SigSpec> sign_bits(sign_bit_cnt, SigSpec(is_signed)); // this is incorrect for the signed case
    SigSpec extended_sig_spec;
    for(SigSpec sig_spec : sign_bits){
        extended_sig_spec.append(sig_spec);
    }
    extended_sig_spec.append(wire);
    mod->connect(extended_sig_spec, extended_wire);
    return extended_wire;
}

Wire* addBinaryOperationCell(Module *mod, Wire *lhs, Wire *rhs, IdString cellType, string typeWord){
    Cell *cell = mod->addCell(mod->uniquify(RTLIL::escape_id(typeWord + "_cell")), cellType);
    cell->setParam(ID::A_SIGNED, false);
    cell->setParam(ID::B_SIGNED, false);

    int max_width = max(lhs->width, rhs->width);
    cell->setParam(ID::A_WIDTH, lhs->width);
    cell->setParam(ID::B_WIDTH, rhs->width);
    cell->setParam(ID::Y_WIDTH, max_width);
    
    cell->setPort(ID::A, lhs);
    cell->setPort(ID::B, rhs);

    Wire *out_wire = mod->addWire(mod->uniquify(RTLIL::escape_id(typeWord + "_result")), max_width);

    cell->setPort(ID::Y, out_wire);

    return out_wire;

}

class BasicExpression {

public:
    virtual ~BasicExpression() = default;
    virtual double evaluate(vector<unsigned int> values) const = 0;
    virtual int size() const = 0;
    virtual Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const = 0;
    virtual string to_string(vector<Wire*> variables) const = 0;
};

class Variable : public BasicExpression {
    unsigned int index;
public:
    explicit Variable(unsigned int index) :index(index) {}

    double evaluate(vector<unsigned int> values) const override {
        assert(index < ((unsigned int) values.size()));
        return values[index];
    }

    int size() const override{
        return 1;
    }

    Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const {
        assert(index < variables.size());
        Wire* var_wire = mod->addWire(mod->uniquify(RTLIL::escape_id("expression_wire")), variables[index]);
        var_wire->port_input = var_wire->port_output = false;
        var_wire->port_id = 0;
        // assert((!var_wire->port_input) && (!var_wire->port_output));
        mod->connect(var_wire, variables[index]);
        return extend_wire(mod, var_wire, 32, false);
    }

    string to_string(vector<Wire*> variables) const {
        return variables[index]->name.str();
    }
};

// Addition expression
class Addition : public BasicExpression {
    BasicExpression *left, *right;
public:
    Addition(BasicExpression *left, BasicExpression *right)
        : left(left), right(right) {}

    double evaluate(vector<unsigned int> values) const override {
        return left->evaluate(values) + right->evaluate(values);
    }

    int size() const override{
        return 1 + left->size() + right->size();
    }

    Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const {
        Wire* lhs = left->convert_to_rtlil(mod, variables);
        Wire* rhs = right->convert_to_rtlil(mod, variables);
        return addBinaryOperationCell(mod, lhs, rhs, ID($add), "addition");
    }

    string to_string(vector<Wire*> variables) const {
        return "( " + left->to_string(variables) + " ) + ( " + right->to_string(variables) + " )";
    }
};

// Subtraction expression
class Subtraction : public BasicExpression {
    BasicExpression *left, *right;
public:
    Subtraction(BasicExpression *left, BasicExpression *right)
        : left(left), right(right) {}

    double evaluate(vector<unsigned int> values) const override {
        return left->evaluate(values) - right->evaluate(values);
    }

    int size() const override{
        return 1 + left->size() + right->size();
    }

    Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const {
        Wire* lhs = left->convert_to_rtlil(mod, variables);
        Wire* rhs = right->convert_to_rtlil(mod, variables);
        return addBinaryOperationCell(mod, lhs, rhs, ID($sub), "subtraction");
    }

    string to_string(vector<Wire*> variables) const {
        return "( " + left->to_string(variables) + " ) - ( " + right->to_string(variables) + " )";
    }
};

// Multiplication expression
class Multiplication : public BasicExpression {
    BasicExpression *left, *right;
public:
    Multiplication(BasicExpression *left, BasicExpression *right)
        : left(left), right(right) {}

    double evaluate(vector<unsigned int> values) const override {
        return left->evaluate(values) * right->evaluate(values);
    }

    int size() const override{
        return 1 + left->size() + right->size();
    }

    Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const {
        Wire* lhs = left->convert_to_rtlil(mod, variables);
        Wire* rhs = right->convert_to_rtlil(mod, variables);
        return addBinaryOperationCell(mod, lhs, rhs, ID($mul), "multiplication");
    }

    string to_string(vector<Wire*> variables) const {
        return "( " + left->to_string(variables) + " ) * ( " + right->to_string(variables) + " )";
    }
};

// TODO: this will leak a lot of memory
vector<BasicExpression*> generate_expressions(int target_size, int variable_count){
    vector<BasicExpression*> ans;
    if(target_size == 1){
        for(int var_id = 0; var_id < variable_count; var_id++){
            ans.push_back(new Variable(var_id));
        }
    }
    else{
        for(int l = 1; target_size - 1 - l >= 1; l++){
            int r = target_size - 1 - l;
            vector<BasicExpression*> lefts = generate_expressions(l, variable_count), rights = generate_expressions(r, variable_count);
            for(auto left : lefts){
                for(auto right : rights){
                    ans.push_back(new Addition(left, right));
                    ans.push_back(new Multiplication(left, right));
                    ans.push_back(new Subtraction(left, right));
                }
            }
        }
    }
    return ans;
}

PRIVATE_NAMESPACE_END
