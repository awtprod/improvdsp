#include "params.hpp"

#include <cstring>

namespace imp {

const ParamInfo kParamInfo[P_COUNT] = {
#define IMP_INFO(name, def, lo, hi, persist, help) {#name, def, lo, hi, persist != 0, help},
    IMP_PARAM_LIST(IMP_INFO)
#undef IMP_INFO
};

int Params::find(const std::string& name) {
    for (int i = 0; i < P_COUNT; ++i)
        if (name == kParamInfo[i].name) return i;
    return -1;
}

}  // namespace imp
