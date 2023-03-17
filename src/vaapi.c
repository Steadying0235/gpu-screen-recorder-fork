#include "../include/vaapi.h"
#include "../include/library_loader.h"
#include <string.h>

bool gsr_vaapi_load(gsr_vaapi *self) {
    memset(self, 0, sizeof(gsr_vaapi));

    dlerror(); /* clear */
    void *lib = dlopen("libva.so.2", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: gsr_vaapi_load failed: failed to load libva.so, error: %s\n", dlerror());
        return false;
    }

    dlsym_assign required_dlsym[] = {
        { (void**)&self->vaExportSurfaceHandle, "vaExportSurfaceHandle" },
        { (void**)&self->vaSyncSurface, "vaSyncSurface" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(lib, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_vaapi_load failed: missing required symbols in libva.so\n");
        goto fail;
    }

    self->library = lib;
    return true;

    fail:
    dlclose(lib);
    memset(self, 0, sizeof(gsr_vaapi));
    return false;
}

void gsr_vaapi_unload(gsr_vaapi *self) {
    if(self->library) {
        dlclose(self->library);
        memset(self, 0, sizeof(gsr_vaapi));
    }
}
