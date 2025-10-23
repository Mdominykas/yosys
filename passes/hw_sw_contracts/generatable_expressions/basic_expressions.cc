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

Wire* addBinaryOperationCell(Module *mod, Wire *lhs, Wire *rhs, IdString cellType, string typeWord){
    Cell *cell = mod->addCell(mod->uniquify(RTLIL::escape_id(typeWord + "_cell")), ID($add));
    cell->setParam(ID::A_SIGNED, false);
    cell->setParam(ID::B_SIGNED, false);

    int max_width = max(lhs->width, rhs->width);
    cell->setParam(ID::A_WIDTH, lhs->width);
    cell->setParam(ID::B_WIDTH, rhs->width);
    cell->setParam(ID::Y_WIDTH, max_width);
    
    cell->setPort(ID::A, lhs);
    cell->setPort(ID::B, rhs);

    Wire *out_wire = mod->addWire(mod->uniquify(RTLIL::escape_id(typeWord + "_output")), max_width);
    return out_wire;

}

class BasicExpression {

public:
    virtual ~BasicExpression() = default;
    virtual double evaluate(vector<unsigned int> values) const = 0;
    virtual int size() const = 0;
    virtual Wire* convert_to_rtlil(Module *mod, vector<Wire*> variables) const = 0;
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
        mod->connect(var_wire, variables[index]);
        return var_wire;
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
