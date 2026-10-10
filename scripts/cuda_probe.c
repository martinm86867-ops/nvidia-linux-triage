#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

/**
 * cuda_probe.c
 * Bare-metal CUDA driver API validation using dynamic loading (libcuda.so.1).
 * Requires zero CUDA SDK / toolkit packages installed.
 *
 * Compilation:
 *   gcc cuda_probe.c -ldl -o cuda_probe
 */

typedef int (*cuInit_t)(unsigned int);
typedef int (*cuDeviceGetCount_t)(int*);
typedef int (*cuDeviceGet_t)(int*, int);
typedef int (*cuDeviceGetName_t)(char*, int, int);
typedef int (*cuCtxCreate_t)(void**, unsigned int, int);
typedef int (*cuCtxDestroy_t)(void*);

int main() {
    printf("[*] Probing CUDA Driver via libcuda.so.1 dynamic loader...\n");
    void* handle = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!handle) {
        printf("[-] Failed to open libcuda.so.1: %s\n", dlerror());
        printf("[-] Ensure NVIDIA driver userland libraries are installed.\n");
        return 1;
    }
    printf("[+] Successfully linked libcuda.so.1\n");

    cuInit_t p_cuInit = (cuInit_t)dlsym(handle, "cuInit");
    cuDeviceGetCount_t p_cuDeviceGetCount = (cuDeviceGetCount_t)dlsym(handle, "cuDeviceGetCount");
    cuDeviceGet_t p_cuDeviceGet = (cuDeviceGet_t)dlsym(handle, "cuDeviceGet");
    cuDeviceGetName_t p_cuDeviceGetName = (cuDeviceGetName_t)dlsym(handle, "cuDeviceGetName");
    cuCtxCreate_t p_cuCtxCreate = (cuCtxCreate_t)dlsym(handle, "cuCtxCreate_v2");
    cuCtxDestroy_t p_cuCtxDestroy = (cuCtxDestroy_t)dlsym(handle, "cuCtxDestroy_v2");

    if (!p_cuInit || !p_cuDeviceGetCount || !p_cuDeviceGet || !p_cuDeviceGetName || !p_cuCtxCreate || !p_cuCtxDestroy) {
        printf("[-] Failed to resolve required CUDA symbols from driver.\n");
        dlclose(handle);
        return 1;
    }

    int res = p_cuInit(0);
    if (res != 0) {
        printf("[-] cuInit(0) failed with error code: %d\n", res);
        if (res == 100) {
            printf("[-] Error 100 = CUDA_ERROR_NO_DEVICE. The kernel driver rejected initialization.\n");
            printf("[-] Most likely cause: GPU is wedged in Xid 154 (GPU Reset Required) state.\n");
        }
        dlclose(handle);
        return 1;
    }
    printf("[+] cuInit(0) succeeded.\n");

    int count = 0;
    res = p_cuDeviceGetCount(&count);
    if (res != 0) {
        printf("[-] cuDeviceGetCount failed with error code: %d\n", res);
        dlclose(handle);
        return 1;
    }

    printf("[+] Detected %d CUDA device(s).\n", count);
    if (count == 0) {
        printf("[-] No supported CUDA devices detected.\n");
        dlclose(handle);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        int dev = 0;
        p_cuDeviceGet(&dev, i);
        char name[256];
        p_cuDeviceGetName(name, sizeof(name), dev);
        printf("[+] Device %d: %s\n", i, name);

        void* ctx = NULL;
        res = p_cuCtxCreate(&ctx, 0, dev);
        if (res != 0) {
            printf("[-] cuCtxCreate failed for device %d (error %d)\n", i, res);
            dlclose(handle);
            return 1;
        }
        printf("[+] CUDA context created successfully on device %d.\n", i);
        p_cuCtxDestroy(ctx);
        printf("[+] CUDA context destroyed cleanly.\n");
    }

    printf("[+] All silicon checks passed. GPU is ready for compute workloads.\n");
    dlclose(handle);
    return 0;
}
