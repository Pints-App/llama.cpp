#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"

#ifdef GGML_USE_CUBLAS
#include "ggml-cuda.h"
#endif

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <cfloat>

struct test_model {
    struct ggml_tensor * q;
    struct ggml_tensor * k;
    struct ggml_tensor * v;
    struct ggml_tensor * msk;
    ggml_backend_t backend = NULL;
    ggml_backend_buffer_t buffer = NULL;
    struct ggml_context * ctx = NULL;
    bool naive_attn = false;
};

static std::vector<float> tensor_to_float(const ggml_tensor * t) {
    std::vector<float> tv;
    tv.reserve(ggml_nelements(t));

    std::vector<uint8_t> buf(ggml_nbytes(t));
    ggml_backend_tensor_get(t, buf.data(), 0, ggml_nbytes(t));

    ggml_type_traits_t tt = ggml_internal_get_type_traits(t->type);
    size_t bs = ggml_blck_size(t->type);
    std::vector<float> vq(ggml_blck_size(t->type));
    bool quantized = ggml_is_quantized(t->type);

    // access elements by index to avoid gaps in views
    for (int64_t i3 = 0; i3 < t->ne[3]; i3++) {
        for (int64_t i2 = 0; i2 < t->ne[2]; i2++) {
            for (int64_t i1 = 0; i1 < t->ne[1]; i1++) {
                for (int64_t i0 = 0; i0 < t->ne[0]; i0 += bs) {
                    size_t i = i3*t->nb[3] + i2*t->nb[2] + i1*t->nb[1] + i0/bs*t->nb[0];
                    if (t->type == GGML_TYPE_F16) {
                        tv.push_back(ggml_fp16_to_fp32(*(ggml_fp16_t*)&buf[i]));
                    } else if (t->type == GGML_TYPE_F32) {
                        tv.push_back(*(float *) &buf[i]);
                    } else if (t->type == GGML_TYPE_I32) {
                        tv.push_back((float)*(int32_t *) &buf[i]);
                    } else if (t->type == GGML_TYPE_I16) {
                        tv.push_back((float)*(int16_t *) &buf[i]);
                    } else if (t->type == GGML_TYPE_I8) {
                        tv.push_back((float)*(int8_t *) &buf[i]);
                    } else if (quantized) {
                        std::vector<float> vq(ggml_blck_size(t->type));
                        tt.to_float(&buf[i], vq.data(), ggml_blck_size(t->type));
                        tv.insert(tv.end(), vq.begin(), vq.end());
                    } else {
                        GGML_ASSERT(false);
                    }
                }
            }
        }
    }

    return tv;
}

// accept FLT_MAX as infinity
static bool isinf_or_max(float f) {
    return std::isinf(f) || f == FLT_MAX || f == -FLT_MAX;
}

// normalized mean squared error = mse(a, b) / mse(a, 0)
static double nmse(const float * a, const float * b, size_t n) {
    double mse_a_b = 0.0;
    double mse_a_0 = 0.0;

    for (size_t i = 0; i < n; i++) {
        float a_i = a[i];
        float b_i = b[i];

        mse_a_b += (a_i - b_i) * (a_i - b_i);
        mse_a_0 += a_i * a_i;
    }

    return mse_a_b / mse_a_0;
}

void ggml_tensor_set_f32(struct ggml_tensor* tensor, float value, int l, int k = 0, int j = 0, int i = 0) {
    GGML_ASSERT(tensor->nb[0] == sizeof(float));
    *(float*)((char*)(tensor->data) + i * tensor->nb[3] + j * tensor->nb[2] + k * tensor->nb[1] + l * tensor->nb[0]) = value;
}

float ggml_tensor_get_f32(const ggml_tensor* tensor, int l, int k = 0, int j = 0, int i = 0) {
    GGML_ASSERT(tensor->nb[0] == sizeof(float));
    return *(float*)((char*)(tensor->data) + i * tensor->nb[3] + j * tensor->nb[2] + k * tensor->nb[1] + l * tensor->nb[0]);
}

void load_model(test_model & model, int head_dim, int batch_size, int kv_size, int num_heads) {
    float* query = new float[head_dim * batch_size * num_heads];
    float* key = new float[head_dim * kv_size * num_heads];
    float* value = new float[head_dim * kv_size * num_heads];
    float* mask = new float[kv_size * GGML_PAD(batch_size, GGML_KQ_MASK_PAD)];

    for(int i = 0; i < head_dim*batch_size*num_heads;i ++) {
        query[i] = i % 3 ? 2.0f : 1.5f;
    }

    for(int i = 0; i < head_dim*kv_size*num_heads;i ++) {
        key[i] = i % 3 ? 2.3f : 2.8f;
        value[i] = i % 3 ? 3.5f : 1.5f;
    }

    for(int i = 0; i < GGML_PAD(batch_size, GGML_KQ_MASK_PAD)*kv_size;i ++) {
        mask[i] = i % 3 ? 1.0f : 1.5f;
    }

    size_t buffer_size = 0;
    {
        buffer_size += head_dim * batch_size * num_heads * ggml_type_sizef(GGML_TYPE_F32); // tensor q
        buffer_size += head_dim * kv_size * num_heads * ggml_type_sizef(GGML_TYPE_F16); // tensor k
        buffer_size += head_dim * kv_size * num_heads * ggml_type_sizef(GGML_TYPE_F16); // tensor v
        buffer_size += GGML_PAD(batch_size, GGML_KQ_MASK_PAD) * kv_size * ggml_type_sizef(GGML_TYPE_F16); // tensor mask
        buffer_size += 1024;
    }

    printf("%s: ggml tensor size    = %d bytes\n", __func__, (int) sizeof(ggml_tensor));
    printf("%s: backend buffer size = %0.2f MB\n", __func__, (buffer_size/ 1024.f/ 1024.f));

    int num_tensors = 4;
    struct ggml_init_params params {
            /*.mem_size   =*/ ggml_tensor_overhead() * num_tensors,
            /*.mem_buffer =*/ NULL,
            /*.no_alloc   =*/ true,
    };

    // initialize the backend
#ifdef GGML_USE_CUBLAS
    fprintf(stderr, "%s: using CUDA backend\n", __func__);
    model.backend = ggml_backend_cuda_init(0);
    if (!model.backend) {
        fprintf(stderr, "%s: ggml_backend_cuda_init() failed\n", __func__);
    }
#endif

    if(!model.backend) {
        // fallback to CPU backend
        model.backend = ggml_backend_cpu_init();
    }

    model.buffer = ggml_backend_alloc_buffer(model.backend, buffer_size);

    // create context
    model.ctx = ggml_init(params);

    // create tensors
    model.q = ggml_new_tensor_3d(model.ctx, GGML_TYPE_F32, head_dim, batch_size, num_heads);
    model.k = ggml_new_tensor_3d(model.ctx, GGML_TYPE_F16, head_dim, kv_size, num_heads);
    model.v = ggml_new_tensor_3d(model.ctx, GGML_TYPE_F16, head_dim, kv_size, num_heads);
    model.msk = ggml_new_tensor_2d(model.ctx, GGML_TYPE_F16, kv_size, GGML_PAD(batch_size, GGML_KQ_MASK_PAD));

    // create a allocator
    ggml_allocr * alloc = ggml_allocr_new_from_buffer(model.buffer);

    // alloc memory
    ggml_allocr_alloc(alloc, model.q);
    ggml_allocr_alloc(alloc, model.k);
    ggml_allocr_alloc(alloc, model.v);
    ggml_allocr_alloc(alloc, model.msk);

    ggml_fp16_t* k_f16 = new ggml_fp16_t[head_dim * kv_size * num_heads];
    ggml_fp16_t* v_f16 = new ggml_fp16_t[head_dim * kv_size * num_heads];
    ggml_fp16_t* m_f16 = new ggml_fp16_t[GGML_PAD(batch_size, GGML_KQ_MASK_PAD) * kv_size];

    ggml_fp32_to_fp16_row(key, k_f16, head_dim * kv_size * num_heads);
    ggml_fp32_to_fp16_row(value, v_f16, head_dim * kv_size * num_heads);
    ggml_fp32_to_fp16_row(mask, m_f16, GGML_PAD(batch_size, GGML_KQ_MASK_PAD) * kv_size);

    ggml_backend_tensor_set(model.q, query, 0, ggml_nbytes(model.q));
    ggml_backend_tensor_set(model.k, k_f16, 0, ggml_nbytes(model.k));
    ggml_backend_tensor_set(model.v, v_f16, 0, ggml_nbytes(model.v));
    ggml_backend_tensor_set(model.msk, m_f16, 0, ggml_nbytes(model.msk));
}

struct ggml_cgraph * build_graph(const test_model& model, struct ggml_allocr * allocr) {
    static size_t buf_size = ggml_tensor_overhead()*GGML_DEFAULT_GRAPH_SIZE + ggml_graph_overhead();
    static std::vector<uint8_t> buf(buf_size);

    struct ggml_init_params params0 = {
        /*.mem_size   =*/ buf_size,
        /*.mem_buffer =*/ buf.data(),
        /*.no_alloc   =*/ true, // the tensors will be allocated later by ggml_allocr_alloc_graph()
    };

    // create a temporally context to build the graph
    struct ggml_context * ctx0 = ggml_init(params0);

    struct ggml_cgraph  * gf = ggml_new_graph(ctx0);

    if(!model.naive_attn) {
        struct ggml_tensor* result = ggml_flash_attn_ext(ctx0, model.q, model.k, model.v, model.msk, 1.0f / sqrtf(model.q->ne[0]));
        ggml_build_forward_expand(gf, result);
    } else {
        struct ggml_tensor* kq = ggml_mul_mat(ctx0, model.k, model.q);
        kq = ggml_soft_max_ext(ctx0, kq, model.msk, 1.0f / sqrtf(model.q->ne[0]));
        kq = ggml_mul_mat(ctx0, ggml_cont(ctx0, ggml_transpose(ctx0, model.v)), kq);
        kq = ggml_permute     (ctx0, kq, 0, 2, 1, 3);
        //kq = ggml_cont_2d     (ctx0, kq, model.q->ne[0] * model.q->ne[2], model.q->ne[1]);
        ggml_build_forward_expand(gf, kq);
    }

    // delete the temporally context used to build the graph
    ggml_free(ctx0);
    return gf;
}

struct ggml_tensor* compute_graph(const test_model & model, ggml_backend_t backend_cpu, struct ggml_allocr * allocr, bool compare_backends) {
    // reset the allocator to free all the memory allocated during the previous inference
    ggml_allocr_reset(allocr);

    struct ggml_cgraph * gf = build_graph(model, allocr);

    // allocate tensors
    ggml_allocr_alloc_graph(allocr, gf);
    int n_threads = 6;

    if (ggml_backend_is_cpu(model.backend)) {
        ggml_backend_cpu_set_n_threads(model.backend, n_threads);
    }

    if(!compare_backends) {
        ggml_backend_graph_compute(model.backend, gf);

        // in this case, the output tensor is the last one in the graph
        return gf->nodes[gf->n_nodes - 1];
    }

    struct callback_userdata {
        bool   ok;
        double max_err;
        ggml_backend_t backend1;
        ggml_backend_t backend2;
    };

    callback_userdata ud {
        true,
        5e-4,
        model.backend,
        backend_cpu
    };

    auto callback = [](int index, ggml_tensor * t1, ggml_tensor * t2, void * user_data) -> bool {
        callback_userdata * ud = (callback_userdata *) user_data;
        const char * bn1 = ggml_backend_name(ud->backend1);
        const char * bn2 = ggml_backend_name(ud->backend2);

        if (t1->op == GGML_OP_NONE) {
            // sentinels must be unchanged
            std::vector<uint8_t> t1_data(ggml_nbytes(t1));
            std::vector<uint8_t> t2_data(ggml_nbytes(t2));
            ggml_backend_tensor_get(t1, t1_data.data(), 0, ggml_nbytes(t1));
            ggml_backend_tensor_get(t2, t2_data.data(), 0, ggml_nbytes(t2));

            if (memcmp(t1_data.data(), t2_data.data(), ggml_nbytes(t1)) != 0) {
                printf("sentinel mismatch: %s ", t1->name);
                ud->ok = false;
                return true;
            }
        }

        std::vector<float> f1 = tensor_to_float(t1);
        std::vector<float> f2 = tensor_to_float(t2);

        for (size_t i = 0; i < f1.size(); i++) {
            // check for nans
            if (std::isnan(f1[i]) || std::isnan(f2[i])) {
                printf("[%s] NaN at index %zu (%s=%f %s=%f) ", ggml_op_desc(t1), i, bn1, f1[i], bn2, f2[i]);
                ud->ok = false;
                return true;
            }
            // check for infs: both must be inf of the same sign, or both must be finite
            if (isinf_or_max(f1[i]) || isinf_or_max(f2[i])) {
                if (isinf_or_max(f1[i]) && isinf_or_max(f2[i])) {
                    if (std::signbit(f1[i]) != std::signbit(f2[i])) {
                        printf("[%s] inf sign mismatch: %s=%f %s=%f ", ggml_op_desc(t1), bn1, f1[i], bn2, f2[i]);
                        ud->ok = false;
                        return true;
                    }
                } else {
                    printf("[%s] inf mismatch: %s=%f %s=%f ", ggml_op_desc(t1), bn1, f1[i], bn2, f2[i]);
                    ud->ok = false;
                    return true;
                }
            }
        }

        double err = nmse(f1.data(), f2.data(), f1.size());
        if (err > ud->max_err) {
            printf("[%s] NMSE = %.9f > %.9f ", ggml_op_desc(t1), err, ud->max_err);
            ud->ok = false;
        }

        return true;

        GGML_UNUSED(index);
    };

    printf("\nTesting Flash Attention - comparing backends: ");

    const bool cmp_ok = ggml_backend_compare_graph_backend(model.backend, backend_cpu, gf, callback, &ud);
    if (ud.ok && cmp_ok) {
        printf("\033[1;32mOK\033[0m\n");
        return NULL;
    }

    printf("\033[1;31mFAIL\033[0m\n");
    return NULL;
}

int main(int argc, char ** argv)
{
    bool compare_backend = false;
    test_model model;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "comp") == 0) {
            compare_backend = true;
        } else if (strcmp(argv[i], "naive") == 0) {
            model.naive_attn = true;
        }
    }

    ggml_time_init();

    //load_model(model, 16, 32, 128, 2);
    load_model(model, 64, 2048, 4096, 32);

    ggml_backend_buffer_t buf_compute; // for compute
    struct ggml_allocr * allocr = NULL;

    {
        allocr = ggml_allocr_new_measure_from_backend(model.backend);

        //create the worst case graph for memory usage estimation
        struct ggml_cgraph * gf = build_graph(model, allocr);
        size_t mem_size = ggml_allocr_alloc_graph(allocr, gf);
        ggml_allocr_free(allocr);

        // compute the required memory
        buf_compute = ggml_backend_alloc_buffer(model.backend, mem_size);
        allocr = ggml_allocr_new_from_buffer(buf_compute);
        fprintf(stderr, "%s: compute buffer size: %.2f MB\n", __func__, mem_size/1024.0f/1024.0f);
    }

    ggml_backend_t backend_cpu = ggml_backend_cpu_init();
    uint64_t compute_time_us__ = ggml_time_us();
    struct ggml_tensor * result = compute_graph(model, backend_cpu, allocr, compare_backend);
    if(!compare_backend) {
        ggml_backend_synchronize(model.backend);
        printf("computing time: %.4f ms\n", (ggml_time_us() - compute_time_us__) / 1000.0);
        float* data = new float[ggml_nelements(result)];
        ggml_backend_tensor_get(result, data, 0, ggml_nbytes(result));
        printf("\nPerforming test (%zu):\n", ggml_nelements(result));

        int elements = ggml_nelements(result) > 1024 ? 1024 : ggml_nelements(result);

        for(int i = 0; i < elements; i ++) {
            if(i > 0 && (i % 16 == 0)) {
                printf("\n");
            }
            if(i > 0 && (i % (16 * 32) == 0)) {
                printf("\n\n");
            }
            printf("%2.4f ", data[i]);
        }
    }

    ggml_free(model.ctx);

    ggml_backend_buffer_free(model.buffer);
    ggml_backend_buffer_free(buf_compute);
    ggml_backend_free(model.backend);
    return 0;
}
