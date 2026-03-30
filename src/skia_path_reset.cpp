/*
 * skia_path_reset.cpp — Add SkPath::reset() wrapper missing from skia_c
 *
 * Avoids malloc/free churn on every beginPath() call.
 */

#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>

#define PATH_CAST reinterpret_cast<SkPath*>(c_path)

extern "C" {

typedef struct skiac_path skiac_path;

void skiac_path_reset(skiac_path* c_path) {
    if (c_path) {
        PATH_CAST->reset();
    }
}

}
