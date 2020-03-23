#pragma once

static inline void atomic_add_f64(volatile double* global_value, double addend)
{
    uint64_t expected_value, new_value;
    do {
        double old_value = *global_value;
        expected_value = _castf64_u64(old_value);
        new_value = _castf64_u64(old_value + addend);
    } while (!__sync_bool_compare_and_swap((volatile uint64_t*)global_value,
                                           expected_value, new_value));
}

static inline void direct_add_block(double *dst, int ldd, double *src, int lds, int nrows, int ncols)
{
    for (int irow = 0; irow < nrows; irow++)
    {
        int dst_base = irow * ldd;
        int src_base = irow * lds;
        for (int icol = 0; icol < ncols; icol++)
            dst[dst_base + icol] += src[src_base + icol];
    }
}

static inline void atomic_add_vector(double *dst, double *src, int length)
{
    for (int i = 0; i < length; i++)
        atomic_add_f64(&dst[i], src[i]);
}

static inline void direct_add_vector(double *dst, double *src, int length)
{
    PRAGMA_SIMD
    for (int i = 0; i < length; i++) 
        dst[i] += src[i];
}

static inline void update_global_vectors(
    int write_P, int dimM, int dimN, int dimP, int dimQ,
    double *K_MP, double *K_MP_buf, double *K_NP, double *K_NP_buf, double *J_PQ, double *J_PQ_buf,
    double *K_MQ, double *K_MQ_buf, double *K_NQ, double *K_NQ_buf
)
{
    if (write_P)
    {
        direct_add_vector(K_MP, K_MP_buf, dimM * dimP);
        direct_add_vector(K_NP, K_NP_buf, dimN * dimP);
    }
    
    #ifdef DUP_F_PQ_BUF
    direct_add_vector(J_PQ, J_PQ_buf, dimP * dimQ);
    #else
    atomic_add_vector(J_PQ, J_PQ_buf, dimP * dimQ);
    #endif
    
    direct_add_vector(K_MQ, K_MQ_buf, dimM * dimQ);
    direct_add_vector(K_NQ, K_NQ_buf, dimN * dimQ);
}

#define UPDATE_F_OPT_BUFFER_IN_ARGS \
    int tid, int num_dmat, double *integrals, \
    int dimM, int dimN, int dimP, int _dimQ, \
    int flag1, int flag2, int flag3, int load_P, int write_P, \
    int M, int N, int P, int Q,  \
    double *thread_F_M_band_blocks, int thread_M_bank_offset, \
    double *thread_F_N_band_blocks, int thread_N_bank_offset, \
    double *thread_F_PQ_blocks

// Use thread-local buffer to reduce atomic add 
static inline void update_F_opt_buffer(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    int dimQ = _dimQ;

    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;
    
    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P) memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));

    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            int imn = iM * dimN + iN;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0;
            for (int iP = 0; iP < dimP; iP++) 
            {
                int inp = iN * dimP + iP;
                int imp = iM * dimP + iP;
                double vMQ = vMQ_coef * D_NP_buf[inp];
                double vNQ = vNQ_coef * D_MP_buf[imp];
                
                int Ibase = dimQ * (iP + dimP * imn);
                int ipq_base = iP * dimQ;
                int imq_base = iM * dimQ;
                int inq_base = iN * dimQ;
                
                double k_MP = 0.0, k_NP = 0.0;
                
                // dimQ is small, vectorizing short loops may hurt performance since
                // it needs horizon reduction after the loop
                for (int iQ = 0; iQ < dimQ; iQ++) 
                {
                    double I = integrals[Ibase + iQ];
                    
                    j_MN += D_PQ_buf[ipq_base + iQ] * I;
                    k_MP -= D_NQ_buf[inq_base + iQ] * I;
                    k_NP -= D_MQ_buf[imq_base + iQ] * I;
                    J_PQ_buf[ipq_base + iQ] += vPQ * I;
                    K_MQ_buf[imq_base + iQ] -= vMQ * I;
                    K_NQ_buf[inq_base + iQ] -= vNQ * I;
                }
                K_MP_buf[imp] += k_MP * vMP_coef;
                K_NP_buf[inp] += k_NP * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[imn] += j_MN * vMN_coef;
        } // for (int iN = 0; iN < dimN; iN++) 
    } // for (int iM = 0; iM < dimM; iM++) 
    
    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_opt_buffer_Q1(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    const int dimQ = 1;

    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;
    
    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P) memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));
    
    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            const int imn = iM * dimN + iN;
            const int imn_dimP = imn * dimP;
            const int inp_base = iN * dimP;
            const int imp_base = iM * dimP;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0, k_MQ = 0.0, k_NQ = 0.0;
            // Don't vectorize this loop, too short
            for (int iP = 0; iP < dimP; iP++) 
            {
                double vMQ = vMQ_coef * D_NP_buf[inp_base + iP];
                double vNQ = vNQ_coef * D_MP_buf[imp_base + iP];
                
                double I = integrals[imn_dimP + iP];
                
                j_MN += I * D_PQ_buf[iP];
                k_MQ -= vMQ * I;
                k_NQ -= vNQ * I;
                J_PQ_buf[iP * dimQ] += vPQ * I;
                K_MP_buf[imp_base + iP] -= I * D_NQ_buf[iN] * vMP_coef;
                K_NP_buf[inp_base + iP] -= I * D_MQ_buf[iM] * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[iM * dimN + iN] += j_MN * vMN_coef;
            K_MQ_buf[iM * dimQ] += k_MQ;
            K_NQ_buf[iN * dimQ] += k_NQ;
        } // for (int iN = 0; iN < dimN; iN++) 
    } // for (int iM = 0; iM < dimM; iM++) 
    
    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_opt_buffer_Q3(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    const int dimQ = 3;
    
    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;
    
    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P)  memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));

    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            int imn = iM * dimN + iN;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0;
            for (int iP = 0; iP < dimP; iP++) 
            {
                int inp = iN * dimP + iP;
                int imp = iM * dimP + iP;
                double vMQ = vMQ_coef * D_NP_buf[inp];
                double vNQ = vNQ_coef * D_MP_buf[imp];
                
                int Ibase = dimQ * (iP + dimP * imn);
                int ipq_base = iP * dimQ;
                int imq_base = iM * dimQ;
                int inq_base = iN * dimQ;
                
                double k_MP = 0.0, k_NP = 0.0;
                
                #pragma unroll
                for (int iQ = 0; iQ < 3; iQ++) 
                {
                    double I = integrals[Ibase + iQ];
                    j_MN += D_PQ_buf[ipq_base + iQ] * I;
                    k_MP -= D_NQ_buf[inq_base + iQ] * I;
                    k_NP -= D_MQ_buf[imq_base + iQ] * I;
                    J_PQ_buf[ipq_base + iQ] += vPQ * I;
                    K_MQ_buf[imq_base + iQ] -= vMQ * I;
                    K_NQ_buf[inq_base + iQ] -= vNQ * I;
                } 
                K_MP_buf[imp] += k_MP * vMP_coef;
                K_NP_buf[inp] += k_NP * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[imn] += j_MN * vMN_coef;
        } // for (int iN = 0; iN < dimN; iN++)
    } // for (int iM = 0; iM < dimM; iM++) 
    
    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_opt_buffer_Q6(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    const int dimQ = 6;
    
    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;
    
    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P)  memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));

    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            int imn = iM * dimN + iN;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0;
            for (int iP = 0; iP < dimP; iP++) 
            {
                int inp = iN * dimP + iP;
                int imp = iM * dimP + iP;
                double vMQ = vMQ_coef * D_NP_buf[inp];
                double vNQ = vNQ_coef * D_MP_buf[imp];
                
                int Ibase = dimQ * (iP + dimP * imn);
                int ipq_base = iP * dimQ;
                int imq_base = iM * dimQ;
                int inq_base = iN * dimQ;
                
                double k_MP = 0.0, k_NP = 0.0;
                
                #pragma ivdep
                for (int iQ = 0; iQ < 6; iQ++) 
                {
                    double I = integrals[Ibase + iQ];
                    j_MN += D_PQ_buf[ipq_base + iQ] * I;
                    k_MP -= D_NQ_buf[inq_base + iQ] * I;
                    k_NP -= D_MQ_buf[imq_base + iQ] * I;
                    J_PQ_buf[ipq_base + iQ] += vPQ * I;
                    K_MQ_buf[imq_base + iQ] -= vMQ * I;
                    K_NQ_buf[inq_base + iQ] -= vNQ * I;
                }
                K_MP_buf[imp] += k_MP * vMP_coef;
                K_NP_buf[inp] += k_NP * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[imn] += j_MN * vMN_coef;
        } // for (int iM = 0; iM < dimM; iM++) 
    } // for (int iM = 0; iM < dimM; iM++)
    
    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_opt_buffer_Q10(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    const int dimQ = 10;
    
    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;

    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P)  memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));

    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            int imn = iM * dimN + iN;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0;
            for (int iP = 0; iP < dimP; iP++) 
            {
                int inp = iN * dimP + iP;
                int imp = iM * dimP + iP;
                double vMQ = vMQ_coef * D_NP_buf[inp];
                double vNQ = vNQ_coef * D_MP_buf[imp];
                
                int Ibase = dimQ * (iP + dimP * imn);
                int ipq_base = iP * dimQ;
                int imq_base = iM * dimQ;
                int inq_base = iN * dimQ;
                
                double k_MP = 0.0, k_NP = 0.0;
                
                #pragma ivdep
                for (int iQ = 0; iQ < 10; iQ++) 
                {
                    double I = integrals[Ibase + iQ];
                    j_MN += D_PQ_buf[ipq_base + iQ] * I;
                    k_MP -= D_NQ_buf[inq_base + iQ] * I;
                    k_NP -= D_MQ_buf[imq_base + iQ] * I;
                    J_PQ_buf[ipq_base + iQ] += vPQ * I;
                    K_MQ_buf[imq_base + iQ] -= vMQ * I;
                    K_NQ_buf[inq_base + iQ] -= vNQ * I;
                }
                K_MP_buf[imp] += k_MP * vMP_coef;
                K_NP_buf[inp] += k_NP * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[imn] += j_MN * vMN_coef;
        } // for (int iN = 0; iN < dimN; iN++)
    } // for (int iM = 0; iM < dimM; iM++) 
    
    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_opt_buffer_Q15(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    const int dimQ = 15;
    
    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    int required_buf_size = (dimP + dimN + dimM) * dimQ + (dimN + dimM) * dimP + dimM * dimN;
    assert(required_buf_size <= update_F_buf_size); 
    
    double *write_buf = thread_buf;
    
    // Setup buffer pointers
    double *J_MN_buf = write_buf;  write_buf += dimM * dimN;
    double *K_MP_buf = write_buf;  write_buf += dimM * dimP;
    double *K_NP_buf = write_buf;  write_buf += dimN * dimP;
    double *J_PQ_buf = write_buf;  write_buf += dimP * dimQ;
    double *K_NQ_buf = write_buf;  write_buf += dimN * dimQ;
    double *K_MQ_buf = write_buf;  write_buf += dimM * dimQ;

    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    // Reset result buffer
    if (load_P)  memset(K_MP_buf, 0, sizeof(double) * dimP * (dimM + dimN));
    memset(J_PQ_buf, 0, sizeof(double) * dimQ * (dimM + dimN + dimP));

    double vPQ_coef = 2.0 * (flag3 + flag5 + flag6 + flag7);
    double vMQ_coef = (flag2 + flag6) * 1.0;
    double vNQ_coef = (flag4 + flag7) * 1.0;
    double vMN_coef = 2.0 * (1 + flag1 + flag2 + flag4);
    double vMP_coef = (1 + flag3) * 1.0;
    double vNP_coef = (flag1 + flag5) * 1.0;

    // Start computation
    for (int iM = 0; iM < dimM; iM++) 
    {
        for (int iN = 0; iN < dimN; iN++) 
        {
            int imn = iM * dimN + iN;
            double vPQ = vPQ_coef * D_MN_buf[imn];
            double j_MN = 0.0;
            for (int iP = 0; iP < dimP; iP++) 
            {
                int inp = iN * dimP + iP;
                int imp = iM * dimP + iP;
                double vMQ = vMQ_coef * D_NP_buf[inp];
                double vNQ = vNQ_coef * D_MP_buf[imp];
                
                int Ibase = dimQ * (iP + dimP * imn);
                int ipq_base = iP * dimQ;
                int imq_base = iM * dimQ;
                int inq_base = iN * dimQ;
                
                double k_MP = 0.0, k_NP = 0.0;
                
                #pragma ivdep
                for (int iQ = 0; iQ < 15; iQ++) 
                {
                    double I = integrals[Ibase + iQ];
                    j_MN += D_PQ_buf[ipq_base + iQ] * I;
                    k_MP -= D_NQ_buf[inq_base + iQ] * I;
                    k_NP -= D_MQ_buf[imq_base + iQ] * I;
                    J_PQ_buf[ipq_base + iQ] += vPQ * I;
                    K_MQ_buf[imq_base + iQ] -= vMQ * I;
                    K_NQ_buf[inq_base + iQ] -= vNQ * I;
                }
                K_MP_buf[imp] += k_MP * vMP_coef;
                K_NP_buf[inp] += k_NP * vNP_coef;
            } // for (int iP = 0; iP < dimP; iP++) 
            J_MN_buf[imn] += j_MN * vMN_coef;
        } // for (int iN = 0; iN < dimN; iN++)
    } // for (int iM = 0; iM < dimM; iM++) 

    // Update to the global array using atomic_add_f64()
    update_global_vectors(
        write_P, dimM, dimN, dimP, dimQ, 
        K_MP, K_MP_buf, K_NP, K_NP_buf, J_PQ, J_PQ_buf,
        K_MQ, K_MQ_buf, K_NQ, K_NQ_buf
    );
}

static inline void update_F_1111(UPDATE_F_OPT_BUFFER_IN_ARGS)
{
    int flag4 = (flag1 == 1 && flag2 == 1) ? 1 : 0;
    int flag5 = (flag1 == 1 && flag3 == 1) ? 1 : 0;
    int flag6 = (flag2 == 1 && flag3 == 1) ? 1 : 0;
    int flag7 = (flag4 == 1 && flag3 == 1) ? 1 : 0;
    
    double *J_PQ = thread_F_PQ_blocks + (mat_block_ptr[P * nshells + Q] - F_PQ_offset);
    double *K_MP = thread_F_M_band_blocks + mat_block_ptr[M * nshells + P] - thread_M_bank_offset; 
    double *K_NP = thread_F_N_band_blocks + mat_block_ptr[N * nshells + P] - thread_N_bank_offset;
    double *K_MQ = thread_F_M_band_blocks + mat_block_ptr[M * nshells + Q] - thread_M_bank_offset;
    double *K_NQ = thread_F_N_band_blocks + mat_block_ptr[N * nshells + Q] - thread_N_bank_offset;
    
    double *D_MN_buf = D_blocks + mat_block_ptr[M * nshells + N];
    double *D_PQ_buf = D_blocks + mat_block_ptr[P * nshells + Q];
    double *D_MP_buf = D_blocks + mat_block_ptr[M * nshells + P];
    double *D_NP_buf = D_blocks + mat_block_ptr[N * nshells + P];
    double *D_MQ_buf = D_blocks + mat_block_ptr[M * nshells + Q];
    double *D_NQ_buf = D_blocks + mat_block_ptr[N * nshells + Q];

    double *thread_buf = update_F_buf + tid * update_F_buf_size;
    double I = integrals[0];

    double vMN = 2.0 * (1 + flag1 + flag2 + flag4) * D_PQ_buf[0] * I;
    double vPQ = 2.0 * (flag3 + flag5 + flag6 + flag7) * D_MN_buf[0] * I;
    double vMP = (1 + flag3) * D_NQ_buf[0] * I;
    double vNP = (flag1 + flag5) * D_MQ_buf[0] * I;
    double vMQ = (flag2 + flag6) * D_NP_buf[0] * I;
    double vNQ = (flag4 + flag7) * D_MP_buf[0] * I;
    
    //atomic_add_f64(&J_MN[0], vMN);
    thread_buf[0] += vMN;
    atomic_add_f64(&J_PQ[0], vPQ);
    //atomic_add_f64(&K_MP[0], -vMP);
    //atomic_add_f64(&K_NP[0], -vNP);
    //atomic_add_f64(&K_MQ[0], -vMQ);
    //atomic_add_f64(&K_NQ[0], -vNQ);
    K_MP[0] -= vMP;
    K_NP[0] -= vNP;
    K_MQ[0] -= vMQ;
    K_NQ[0] -= vNQ;
}

// See update_F_orig.h for the original implementation of update_F()

