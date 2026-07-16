// Stub source for the library target — provides version symbols.
#include "nestor/version.hpp"

namespace nestor {

const char* get_version() { return kVersion.data(); }
const char* get_project_name() { return kProjectName.data(); }

}  // namespace nestor
