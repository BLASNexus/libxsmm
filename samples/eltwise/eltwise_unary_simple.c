/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
*               Friedrich Schiller University Jena - All rights reserved.     *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/libxsmm/libxsmm/                    *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Evangelos Georganas, Alexander Heinecke (Intel Corp.), Antonio Noack (FSU Jena)
******************************************************************************/
#include "eltwise_common.h"

#define EPS 1.19209290e-03F

#define RND_RNE 0
#define RND_STOCHASTIC 1

#define NO_BCAST 0
#define ROW_BCAST 1
#define COL_BCAST 2
#define SCALAR_BCAST 3

#define COPY_OP 1
#define XOR_OP 2
#define X2_OP 3
#define SQRT_OP 4
#define TANH_OP 7
#define TANH_INV_OP 8
#define SIGMOID_OP 9
#define SIGMOID_INV_OP 10
#define GELU_OP 11
#define GELU_INV_OP 12
#define NEGATE_OP 13
#define INC_OP 14
#define RCP_OP 15
#define RCP_SQRT_OP 16
#define EXP_OP 17
#define REPLICATE_COL_VAR 27
#define UNZIP 42
#define FP32_TO_BF16X2 64
#define FP32_TO_BF16X3 65
#if 1
#define USE_ZERO_RNG_STATE_UNITTEST
#endif

unsigned int is_reference_kernel = 0;

LIBXSMM_INLINE
void reference_unpack_32bit_to_2x16bit_blocks(libxsmm_blasint M, libxsmm_blasint N, libxsmm_blasint ldi, libxsmm_blasint ldo, char *in_char, char *out_char, long long offset) {
  float *in = (float*)in_char;
  libxsmm_bfloat16 *out_lo = (libxsmm_bfloat16*)out_char;
  libxsmm_bfloat16 *out_hi = (libxsmm_bfloat16*)((char*)out_char + offset);
  libxsmm_blasint i, j;
  for (j = 0; j < N; j++) {
    for (i = 0; i < M; i++) {
      libxsmm_bfloat16_f32 bf16_hp;
      bf16_hp.f = in[j * ldi + i];
      out_lo[j * ldo + i] = bf16_hp.i[0];
      out_hi[j * ldo + i] = bf16_hp.i[1];
    }
  }
}

LIBXSMM_INLINE
void adjust_input_for_fp8_rcp_family( libxsmm_datatype dtype_in, void *in, libxsmm_blasint ldi, libxsmm_blasint N ) {
  float test_vals[20] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0 };
  float *in_f  = (float*) libxsmm_aligned_malloc(sizeof(float)*N*ldi, 64);
  float *in_use;
  libxsmm_blasint i, j;
  if (dtype_in == LIBXSMM_DATATYPE_HF8) {
    libxsmm_convert_hf8_f32( (libxsmm_hfloat8*)in, in_f, N*ldi );
    in_use = in_f;
  } else if (dtype_in == LIBXSMM_DATATYPE_BF8) {
    libxsmm_convert_bf8_f32( (libxsmm_bfloat8*)in, in_f, N*ldi );
    in_use = in_f;
  } else {
    in_use = (float*)in;
  }
  for (j = 0; j < N; j++) {
    for (i = 0; i < ldi; i++) {
      in_use[j*ldi+i] = test_vals[(j*ldi+i)%20];
    }
  }
  if (dtype_in == LIBXSMM_DATATYPE_HF8) {
    libxsmm_rne_convert_fp32_hf8( in_f, (libxsmm_hfloat8*)in, N*ldi );
  } else if (dtype_in == LIBXSMM_DATATYPE_HF8) {
    libxsmm_rne_convert_fp32_bf8( in_f, (libxsmm_bfloat8*)in, N*ldi );
  }
  libxsmm_free(in_f);
}

LIBXSMM_INLINE
float fsigmoid(float x) {
  return (LIBXSMM_TANHF(x/2.0f) + 1.0f)/2.0f;
}

LIBXSMM_INLINE
float fsigmoid_inv(float x) {
  return fsigmoid(x) * (1.0f-fsigmoid(x));
}

LIBXSMM_INLINE
float tanh_inv(float x) {
  return 1.0f-LIBXSMM_TANHF(x)*LIBXSMM_TANHF(x);
}

LIBXSMM_INLINE
float gelu(float x) {
  return (LIBXSMM_ERFF(x/LIBXSMM_SQRTF(2.0f)) + 1.0f)*0.5f*x;
}

LIBXSMM_INLINE
float gelu_inv(float x) {
  return (0.5f + 0.5f * LIBXSMM_ERFF(x/LIBXSMM_SQRTF(2.0f)) + x/(LIBXSMM_SQRTF(2.0f*M_PI)) * LIBXSMM_EXPF(-0.5f*x*x) );
}

LIBXSMM_INLINE
double fp64_unary_compute(double in, unsigned int op) {
  double res = 0;
  if ( op == COPY_OP) {
    res = in;
  } else if ( op == NEGATE_OP) {
    res = -1.0 * in;
  } else if (op == X2_OP) {
    res = in * in;
  } else if (op == XOR_OP) {
    res = 0;
  } else if (op == SQRT_OP) {
    res = sqrt(in);
  } else if (op == INC_OP) {
    res = in + 1.0;
  } else if (op == RCP_OP) {
    res = 1.0/in;
  } else if (op == RCP_SQRT_OP) {
    res = 1.0/sqrt(in);
  } else {
    printf("Invalid OP\n");
    exit(-1);
  }

  return res;
}

LIBXSMM_INLINE
float fp32_unary_compute(float in, unsigned int op) {
  float res = 0;

  if ( op == COPY_OP || op == REPLICATE_COL_VAR) {
    res = in;
  } else if ( op == NEGATE_OP) {
    res = -1.0f * in;
  } else if (op == X2_OP) {
    res = in * in;
  } else if (op == XOR_OP) {
    res = 0;
  } else if (op == TANH_OP) {
    res = LIBXSMM_TANHF(in);
  } else if (op == SIGMOID_OP) {
    res = fsigmoid(in);
  } else if (op == GELU_OP) {
    res = gelu(in);
  } else if (op == GELU_INV_OP) {
    res = gelu_inv(in);
  } else if (op == TANH_INV_OP) {
    res = tanh_inv(in);
  } else if (op == SIGMOID_INV_OP) {
    res = fsigmoid_inv(in);
  } else if (op == SQRT_OP) {
    res = LIBXSMM_SQRTF(in);
  } else if (op == INC_OP) {
    res = in + 1.0f;
  } else if (op == RCP_OP) {
    res = 1.0f/in;
  } else if (op == RCP_SQRT_OP) {
    res = 1.0f/LIBXSMM_SQRTF(in);
  } else if (op == EXP_OP) {
    res = LIBXSMM_EXPF(in);
  } else {
    printf("Invalid OP\n");
    exit(-1);
  }

  return res;
}

LIBXSMM_INLINE
void set_opname(unsigned int op, char *opname) {
  if ( op == COPY_OP ) {
    sprintf(opname, "copy");
  } else if ( op == REPLICATE_COL_VAR ) {
    sprintf(opname, "replicate_col_var");
  } else if ( op == X2_OP ) {
    sprintf(opname, "x2");
  } else if ( op == XOR_OP ) {
    sprintf(opname, "xor");
  } else if ( op == TANH_OP ) {
    sprintf(opname, "tanh");
  } else if ( op == SIGMOID_OP ) {
    sprintf(opname, "sigmoid");
  } else if ( op == GELU_OP ) {
    sprintf(opname, "gelu");
  } else if ( op == GELU_INV_OP ) {
    sprintf(opname, "gelu_inv");
  } else if ( op == TANH_INV_OP ) {
    sprintf(opname, "tanh_inv");
  } else if ( op == SIGMOID_INV_OP ) {
    sprintf(opname, "sigmoid_inv");
  } else if ( op == SQRT_OP ) {
    sprintf(opname, "sqrt");
  } else if ( op == NEGATE_OP ) {
    sprintf(opname, "negate");
  } else if (op == INC_OP) {
    sprintf(opname, "inc");
  } else if (op == RCP_OP) {
    sprintf(opname, "reciprocal");
  } else if (op == RCP_SQRT_OP) {
    sprintf(opname, "reciprocal sqrt");
  } else if (op == EXP_OP) {
    sprintf(opname, "exp");
  } else if (op == UNZIP) {
    sprintf(opname, "unpack to blocks");
  } else if (op == FP32_TO_BF16X2) {
    sprintf(opname, "FP32 decomp to BF16x2");
  } else if (op == FP32_TO_BF16X3) {
    sprintf(opname, "FP32 decomp to BF16x3");
  } else {
    printf("Invalid OP\n");
    exit(-1);
  }
}

LIBXSMM_INLINE
void set_unarytype(unsigned int op, libxsmm_meltw_unary_type *type) {
  libxsmm_meltw_unary_type  unary_type;

  if ( op == COPY_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_IDENTITY;
  } else if ( op == REPLICATE_COL_VAR ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_REPLICATE_COL_VAR;
  } else if ( op == X2_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_X2;
  } else if ( op == XOR_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_XOR;
  } else if ( op == TANH_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_TANH;
  } else if ( op == SIGMOID_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_SIGMOID;
  } else if ( op == GELU_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_GELU;
  } else if ( op == GELU_INV_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_GELU_INV;
  } else if ( op == TANH_INV_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_TANH_INV;
  } else if ( op == SIGMOID_INV_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_SIGMOID_INV;
  } else if ( op == SQRT_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_SQRT;
  } else if ( op == NEGATE_OP ) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_NEGATE;
  } else if (op == INC_OP) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_INC;
  } else if (op == RCP_OP) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_RECIPROCAL;
  } else if (op == EXP_OP) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_EXP;
  } else if (op == RCP_SQRT_OP) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_RECIPROCAL_SQRT;
  } else if (op == UNZIP) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_UNZIP;
  } else if (op == FP32_TO_BF16X2) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_DECOMP_FP32_TO_BF16X2;
  } else if (op == FP32_TO_BF16X3) {
    unary_type = LIBXSMM_MELTW_TYPE_UNARY_DECOMP_FP32_TO_BF16X3;
  } else {
    printf("Invalid OP\n");
    exit(-1);
  }

  *type = unary_type;
}

LIBXSMM_INLINE
void unary_op_gold(const libxsmm_blasint M, const libxsmm_blasint N, const libxsmm_blasint ldi, const libxsmm_blasint ldo, const void *in, void *out,
                   const unsigned int op, const libxsmm_datatype dtype_in, const libxsmm_datatype dtype_out, const libxsmm_datatype dtype_comp, libxsmm_bitfield flags, void *rng_state) {
  libxsmm_blasint i, j;

  if ( ( ((dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_BF16)) ||
         ((dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F16 ))    ) && ((op == COPY_OP) || (op == XOR_OP) || (op == REPLICATE_COL_VAR)) ) {
    for ( j = 0; j < N; ++j ) {
      for ( i = 0; i < M; ++i ) {
        const unsigned short* bf_in = (const unsigned short*)in;
        unsigned short* bf_out = (unsigned short*)out;
        if ( (op == COPY_OP) || (op == REPLICATE_COL_VAR) ) {
          bf_out[(j*ldo) + i] = bf_in[(j*ldi) + i];
        } else if ( op == XOR_OP ) {
          bf_out[(j*ldo) + i] = 0;
        } else {
          /* should not happen */
        }
      }
    }
  } else if ( ( ((dtype_in == LIBXSMM_DATATYPE_BF8) && (dtype_out == LIBXSMM_DATATYPE_BF8) && (dtype_comp == LIBXSMM_DATATYPE_BF8)) ||
                ((dtype_in == LIBXSMM_DATATYPE_HF8) && (dtype_out == LIBXSMM_DATATYPE_HF8) && (dtype_comp == LIBXSMM_DATATYPE_HF8))    ) && ((op == COPY_OP) || (op == XOR_OP) || (op == REPLICATE_COL_VAR)) ) {
    for ( j = 0; j < N; ++j ) {
      for ( i = 0; i < M; ++i ) {
        const unsigned char* bf_in = (const unsigned char*)in;
        unsigned char* bf_out = (unsigned char*)out;
        if ( (op == COPY_OP) || (op == REPLICATE_COL_VAR) ) {
          bf_out[(j*ldo) + i] = bf_in[(j*ldi) + i];
        } else if ( op == XOR_OP ) {
          bf_out[(j*ldo) + i] = 0;
        }
      }
    }
  } else if ( dtype_comp == LIBXSMM_DATATYPE_F32 ) {
    unsigned int seed_idx = 0;
    for ( j = 0; j < N; ++j ) {
      for ( i = 0; i < M; ++i ) {
        float in_value = 0.0f;
        float out_value;
        if ( dtype_in == LIBXSMM_DATATYPE_F32 ) {
          const float* f_in = (const float*)in;
          in_value = f_in[(j*ldi) + i];
        } else if ( dtype_in == LIBXSMM_DATATYPE_BF16 ) {
          const libxsmm_bfloat16* bf16_in = (const libxsmm_bfloat16*)in;
          libxsmm_convert_bf16_f32( &(bf16_in[(j*ldi) + i]), &in_value, 1 );
        } else if ( dtype_in == LIBXSMM_DATATYPE_F16 ) {
          const libxsmm_float16* f16_in = (const libxsmm_float16*)in;
          libxsmm_convert_f16_f32( &(f16_in[(j*ldi) + i]), &in_value, 1 );
        } else if ( dtype_in == LIBXSMM_DATATYPE_BF8 ) {
          const libxsmm_bfloat8* bf8_in = (const libxsmm_bfloat8*)in;
          libxsmm_convert_bf8_f32( &(bf8_in[(j*ldi) + i]), &in_value, 1 );
        } else if ( dtype_in == LIBXSMM_DATATYPE_HF8 ) {
          const libxsmm_hfloat8* hf8_in = (const libxsmm_hfloat8*)in;
          libxsmm_convert_hf8_f32( &(hf8_in[(j*ldi) + i]), &in_value, 1 );
        } else {
          /* should not happen */
        }

        out_value = fp32_unary_compute(in_value, op);

        if ( dtype_out == LIBXSMM_DATATYPE_F32 ) {
          float* f_out = (float*)out;
          f_out[(j*ldo) + i] = out_value;
        } else if ( dtype_out == LIBXSMM_DATATYPE_BF16 ) {
          libxsmm_bfloat16* bf16_out = (libxsmm_bfloat16*)out;
          libxsmm_rne_convert_fp32_bf16(&out_value, &(bf16_out[(j*ldo) + i]), 1);
        } else if ( dtype_out == LIBXSMM_DATATYPE_F16 ) {
          libxsmm_float16* f16_out = (libxsmm_float16*)out;
          libxsmm_rne_convert_fp32_f16(&out_value, &(f16_out[(j*ldo) + i]), 1);
        } else if ( dtype_out == LIBXSMM_DATATYPE_BF8 ) {
          libxsmm_bfloat8* bf8_out = (libxsmm_bfloat8*)out;
          if ((flags & LIBXSMM_MELTW_FLAG_UNARY_STOCHASTIC_ROUND) > 0 ) {
            libxsmm_stochastic_convert_fp32_bf8(&out_value, &(bf8_out[(j*ldo) + i]), 1, rng_state, seed_idx);
            seed_idx++;
          } else {
            libxsmm_rne_convert_fp32_bf8(&out_value, &(bf8_out[(j*ldo) + i]), 1);
          }
        } else if ( dtype_out == LIBXSMM_DATATYPE_HF8 ) {
          libxsmm_hfloat8* hf8_out = (libxsmm_hfloat8*)out;
          libxsmm_rne_convert_fp32_hf8(&out_value, &(hf8_out[(j*ldo) + i]), 1);
        } else {
          /* should not happen */
        }
      }
    }
  } else if ( dtype_comp == LIBXSMM_DATATYPE_F64 ) {
    for ( j = 0; j < N; ++j ) {
      for ( i = 0; i < M; ++i ) {
        double in_value = 0.0;
        double out_value;
        const double* d_in = (const double*)in;
        double* d_out = (double*)out;
        in_value = d_in[(j*ldi) + i];
        out_value = fp64_unary_compute(in_value, op);
        d_out[(j*ldo) + i] = out_value;
      }
    }
  } else {
    /* should not happen */
  }
}

LIBXSMM_INLINE
void unary_op_fp32_to_bf16x2_bf16x3( const libxsmm_blasint M, const libxsmm_blasint N, const libxsmm_blasint ldi, const libxsmm_blasint ldo, const unsigned int op, void* in, void* out_gold ) {
  libxsmm_blasint i, j;

  for ( j = 0; j < N; ++j ) {
    for ( i = 0; i < M; ++i ) {
      float in_value = 0.0f;
      libxsmm_bfloat16 out1_value = 0, out2_value = 0, out3_value = 0;
      float ftmp = 0.0f, ftmp2 = 0.0f;
      libxsmm_bfloat16_f32 tmp;
      const float* f_in = (const float*)in;
      libxsmm_bfloat16* bf16_out = (libxsmm_bfloat16*)out_gold;

      in_value = f_in[(j*ldi) + i];

      tmp.f = in_value;
      tmp.i[0] = 0;
      out1_value = tmp.i[1];

      ftmp = in_value - tmp.f;
      if ( op == FP32_TO_BF16X3 ) {
        tmp.f = ftmp;
        tmp.i[0] = 0;
        out2_value = tmp.i[1];

        ftmp2 = ftmp - tmp.f;
        libxsmm_rne_convert_fp32_bf16(&ftmp2, &out3_value, 1);
      } else {
        libxsmm_rne_convert_fp32_bf16(&ftmp,  &out2_value, 1);
      }

      bf16_out[(j*ldo) + i            ] = out1_value;
      bf16_out[(j*ldo) + i + (ldo*N)  ] = out2_value;
      if ( op == FP32_TO_BF16X3 ) {
        bf16_out[(j*ldo) + i + (ldo*N*2)] = out3_value;
      }
    }
  }
}

LIBXSMM_INLINE
int test_unary_op( const libxsmm_blasint M, const libxsmm_blasint N, const libxsmm_blasint ldi, const libxsmm_blasint ldo, const unsigned int op, const unsigned int use_bcast, const libxsmm_datatype dtype_in, const libxsmm_datatype dtype_out, const libxsmm_datatype dtype_comp, const unsigned int rnd_mode ) {
  char *in, *_in;
  char *out, *out_gold;
  unsigned int *rng_state = NULL;
  long long offset = 0;
  unsigned int *rng_state_gold = NULL;
  libxsmm_kernel_info info;
  int ret = EXIT_SUCCESS;
  libxsmm_matdiff_info norms_out;
  libxsmm_meltw_unary_shape unary_shape = libxsmm_create_meltw_unary_shape( M, N, ldi, ldo, dtype_in, dtype_out, dtype_comp );
  libxsmm_meltw_unary_param unary_param /*= { 0 }*/;
  libxsmm_bitfield unary_flags = LIBXSMM_MELTW_FLAG_UNARY_NONE;
  libxsmm_meltw_unary_type unary_type;
  libxsmm_meltwfunction_unary unary_kernel;
  unsigned long long strides[2];
  char opname[256];
  unsigned long long _N = N;
  double error_bound = 0.0;
  const char* matdiff_env;
  libxsmm_blasint N_out = N;

  set_opname(op, opname);
  set_unarytype(op, &unary_type);

  if ( M > ldi && !(use_bcast == ROW_BCAST || use_bcast == SCALAR_BCAST) ) {
    fprintf( stderr, "test_unary_%s %i %i %i: ldi needs to be equal to or bigger than M\n", opname, (int)dtype_in, (int)dtype_out, (int)dtype_comp);
    exit(-1);
  }
  if (M > ldo ) {
    fprintf( stderr, "test_unary_%s %i %i %i: ldo needs to be equal to or bigger than N\n", opname, (int)dtype_in, (int)dtype_out, (int)dtype_comp);
    exit(-1);
  }

  libxsmm_rng_set_seed(1);

  /* setting N_out based on TPP */
  if ( (op == FP32_TO_BF16X2) || (op == FP32_TO_BF16X3) ) {
    N_out *= 3;
  }
  if ( op == UNZIP ) {
    N_out *= 2;
  }

  in        = (char*) libxsmm_aligned_malloc( LIBXSMM_TYPESIZE(dtype_in) *N*ldi, 64 );
  out       = (char*) libxsmm_aligned_malloc( LIBXSMM_TYPESIZE(dtype_out)*N_out*ldo, 64 );
  out_gold  = (char*) libxsmm_aligned_malloc( LIBXSMM_TYPESIZE(dtype_out)*N_out*ldo, 64 );
  _in       = in;

  /* init in */
  init_random_matrix( dtype_in,  in,       1, ldi, N, 0 );
  init_zero_matrix(   dtype_out, out,      1, ldo, N_out );
  init_zero_matrix(   dtype_out, out_gold, 1, ldo, N_out );
  if (unary_type == LIBXSMM_MELTW_TYPE_UNARY_UNZIP) {
    offset = (long long) LIBXSMM_TYPESIZE(dtype_out)*N*ldo;
  }

  if (((op == RCP_OP) || (op == RCP_SQRT_OP)) && ((dtype_in == LIBXSMM_DATATYPE_HF8) || (dtype_out == LIBXSMM_DATATYPE_HF8) )) {
    adjust_input_for_fp8_rcp_family( dtype_in, in, ldi, N  );
  }
  if (((op == RCP_OP) || (op == RCP_SQRT_OP)) && ((dtype_in == LIBXSMM_DATATYPE_BF8) || (dtype_out == LIBXSMM_DATATYPE_BF8) )) {
    adjust_input_for_fp8_rcp_family( dtype_in, in, ldi, N  );
  }

  if (use_bcast != NO_BCAST) {
    if (use_bcast == ROW_BCAST) {
      apply_row_bcast_matrix( dtype_in, in, ldi, M, N );
    }
    if (use_bcast == COL_BCAST) {
      apply_col_bcast_matrix( dtype_in, in, ldi, M, N );
    }
    if (use_bcast == SCALAR_BCAST) {
      apply_scalar_bcast_matrix( dtype_in, in, ldi, M, N );
    }
  }

  if (rnd_mode == RND_STOCHASTIC) {
    unary_flags = LIBXSMM_MELTW_FLAG_UNARY_STOCHASTIC_ROUND;
    rng_state = libxsmm_rng_create_extstate( 555 );
    rng_state_gold = libxsmm_rng_create_extstate( 555 );
#ifdef USE_ZERO_RNG_STATE_UNITTEST
    memset( (void*)rng_state, 0, libxsmm_rng_get_extstate_size() );
    memset( (void*)rng_state_gold, 0, libxsmm_rng_get_extstate_size() );
#endif
  } else {
    unary_flags = LIBXSMM_MELTW_FLAG_UNARY_NONE;
  }

  /* compute out_gold */
  if (unary_type == LIBXSMM_MELTW_TYPE_UNARY_UNZIP) {
    reference_unpack_32bit_to_2x16bit_blocks( M, N, ldi, ldo, in, out_gold, offset);
  } else if ( (op == FP32_TO_BF16X2) || (op == FP32_TO_BF16X3) ) {
    unary_op_fp32_to_bf16x2_bf16x3( M, N, ldi, ldo, op, in, out_gold );
  } else {
    unary_op_gold( M, N, ldi, ldo, in, out_gold, op, dtype_in, dtype_out, dtype_comp, unary_flags, rng_state_gold );
  }
  /* use jited transpose */
  memset( &unary_param, 0, sizeof(libxsmm_meltw_unary_param) );
  unary_param.in.primary  = (void*)_in;
  unary_param.out.primary = (void*)out;
  if (rnd_mode == RND_STOCHASTIC) {
    unary_param.op.secondary = (void*)rng_state;
  }
  if (unary_type == LIBXSMM_MELTW_TYPE_UNARY_REPLICATE_COL_VAR) {
    unary_param.op.primary = (void*)&_N;
  }
  if (unary_type == LIBXSMM_MELTW_TYPE_UNARY_UNZIP) {
    unary_shape.comp_type = LIBXSMM_DATATYPE_IMPLICIT;
    unary_param.out.secondary = (void*)&offset;
  }
  if ( (unary_type == LIBXSMM_MELTW_TYPE_UNARY_DECOMP_FP32_TO_BF16X2) || (unary_type == LIBXSMM_MELTW_TYPE_UNARY_DECOMP_FP32_TO_BF16X3) ) {
    strides[0] = (unsigned long long)(LIBXSMM_TYPESIZE(dtype_out)*ldo*N);
    strides[1] = (unsigned long long)(LIBXSMM_TYPESIZE(dtype_out)*ldo*N*2);
    unary_param.out.secondary = (void*)(&strides[0]);
  }
  if (use_bcast != NO_BCAST) {
    if (use_bcast == ROW_BCAST) {
      unary_flags |= LIBXSMM_MELTW_FLAG_UNARY_BCAST_ROW;
    }
    if (use_bcast == COL_BCAST) {
      unary_flags |= LIBXSMM_MELTW_FLAG_UNARY_BCAST_COL;
    }
    if (use_bcast == SCALAR_BCAST) {
      unary_flags |= LIBXSMM_MELTW_FLAG_UNARY_BCAST_SCALAR;
    }
  }

  unary_flags |= LIBXSMM_MELTW_FLAG_UNARY_NTS_HINT;

  if (unary_type == LIBXSMM_MELTW_TYPE_UNARY_REPLICATE_COL_VAR) {
    unary_shape.n = 0;
    unary_kernel = libxsmm_dispatch_meltw_unary( unary_type, unary_shape, unary_flags );
  } else {
    unary_kernel = libxsmm_dispatch_meltw_unary( unary_type, unary_shape, unary_flags );
  }
  libxsmm_get_kernel_info((const void*) unary_kernel, &info);
  is_reference_kernel = info.is_reference_kernel;
  if ( unary_kernel == NULL ) {
    fprintf( stderr, "JIT for UNARY TPP. Bailing...!\n");
    exit(-1);
  }

  unary_kernel( &unary_param );

  /* populate error bounds */
  if ( op == RCP_OP || op == RCP_SQRT_OP ) {
    if ((dtype_in == LIBXSMM_DATATYPE_BF16 || dtype_out == LIBXSMM_DATATYPE_BF16) && (libxsmm_get_target_archid() >= LIBXSMM_X86_GENERIC) && (libxsmm_get_target_archid() <= LIBXSMM_X86_AVX2_SRF)) {
      error_bound = 0.008;
    } else {
      error_bound = 0.0027;
    }
  } else if ( op == SQRT_OP || op == EXP_OP || op == TANH_OP || op == TANH_INV_OP ||
              op == SIGMOID_OP || op == SIGMOID_INV_OP || op == GELU_OP || op == GELU_INV_OP ) {
    if ( (dtype_in == LIBXSMM_DATATYPE_F32) && (dtype_out == LIBXSMM_DATATYPE_F32) && (dtype_comp == LIBXSMM_DATATYPE_F32) ) {
      error_bound = 0.0007;
    } else if ( dtype_out == LIBXSMM_DATATYPE_BF16 ) {
      error_bound = 0.007;
    } else if ( (dtype_in == LIBXSMM_DATATYPE_F32) && (dtype_out == LIBXSMM_DATATYPE_BF8) )  {
      error_bound = 0.1;
    } else if ( (dtype_in == LIBXSMM_DATATYPE_F32) && (dtype_out == LIBXSMM_DATATYPE_HF8) )  {
      error_bound = 0.1;
    } else {
      error_bound = 0.007;
    }
  } else {
    error_bound = 0.00001;
  }

  /* eventually amend LIBXSMM_MATDIFF output with error bound */
  matdiff_env = getenv("LIBXSMM_MATDIFF");
  if (NULL != matdiff_env) {
    static char matdiff_ext[1024];
    const int nchars = LIBXSMM_SNPRINTF(matdiff_ext, sizeof(matdiff_ext),
      "LIBXSMM_MATDIFF=%s %.17g", matdiff_env, error_bound);
    if (0 < nchars && nchars < (int)sizeof(matdiff_ext)) {
      LIBXSMM_EXPECT(EXIT_SUCCESS == LIBXSMM_PUTENV(matdiff_ext));
    }
  }

#if 0
  for (int i = 0; i < 2; i ++) {
    for (int j = 0; j < 2; j++) {
      printf("%f %f ", *((float *)out_gold + i * ldo + j), *((float *)out + i * ldo + j));
    }
  }
#endif

  /* compare result */
  norms_out = check_matrix(dtype_out, out_gold, out, ldo, M, N_out);
  printf("##########################################\n");
  printf("#   Correctness  - Output                #\n");
  printf("##########################################\n");
  printf("L1 reference  : %.25g\n", norms_out.l1_ref);
  printf("L1 test       : %.25g\n", norms_out.l1_tst);
  printf("L2 abs.error  : %.24f\n", norms_out.l2_abs);
  printf("L2 rel.error  : %.24f\n", norms_out.l2_rel);
  printf("Linf abs.error: %.24f\n", norms_out.linf_abs);
  printf("Linf rel.error: %.24f\n", norms_out.linf_rel);
  printf("Check-norm    : %.24f\n\n", norms_out.normf_rel);

  if ( norms_out.normf_rel > error_bound ) {
    ret = EXIT_FAILURE;
  }

  benchmark_unary(unary_type, unary_shape, unary_flags, unary_param);

  if (rnd_mode == RND_STOCHASTIC) {
    libxsmm_rng_destroy_extstate( rng_state );
    libxsmm_rng_destroy_extstate( rng_state_gold );
  }
  libxsmm_free( out_gold );
  libxsmm_free( out );
  libxsmm_free( in );

  if ( ret == EXIT_SUCCESS ) {
    printf("SUCCESS unary simple %i %i %i\n", (int)dtype_in, (int)dtype_out, (int)dtype_comp);
  } else {
    printf("FAILURE unary simple %i %i %i\n", (int)dtype_in, (int)dtype_out, (int)dtype_comp);
  }

  return ret;
}

int main( int argc, char* argv[] ) {
  char* dt_in = NULL;
  char* dt_out = NULL;
  char* dt_comp = NULL;
  libxsmm_datatype dtype_in;
  libxsmm_datatype dtype_out;
  libxsmm_datatype dtype_comp;
  unsigned char op;
  libxsmm_blasint use_bcast;
  libxsmm_blasint M;
  libxsmm_blasint N;
  libxsmm_blasint ldi;
  libxsmm_blasint ldo;
  libxsmm_blasint valid_op;
  unsigned int rnd_mode = RND_RNE;
  char opname[256];
  int ret = EXIT_FAILURE;

  if ( argc != 11 && argc != 10 ) {
    printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [Opt: rnd_mode: 0/1]\n", argv[0] );
    exit(-1);
  }

  op         = (unsigned char)atoi(argv[1]);
  use_bcast  = atoi(argv[2]);
  dt_in      = argv[3];
  dt_comp    = argv[4];
  dt_out     = argv[5];
  M          = atoi(argv[6]);
  N          = atoi(argv[7]);
  ldi        = atoi(argv[8]);
  ldo        = atoi(argv[9]);

  if (argc > 10 ) {
    rnd_mode   = atoi(argv[10]);
  }

  dtype_in   = char_to_libxsmm_datatype( dt_in );
  dtype_out  = char_to_libxsmm_datatype( dt_out );
  dtype_comp = char_to_libxsmm_datatype( dt_comp );

  set_opname(op, opname);

  valid_op = ( op == COPY_OP || op == X2_OP || op == XOR_OP || op == TANH_OP || op == SIGMOID_OP || op == GELU_OP ||
               op == GELU_INV_OP || op == TANH_INV_OP || op == SIGMOID_INV_OP || op == SQRT_OP || op == NEGATE_OP ||
               op == INC_OP || op == RCP_OP || op == RCP_SQRT_OP || op == EXP_OP || op == REPLICATE_COL_VAR || op == UNZIP) ? 1 : 0;

  if ((rnd_mode == RND_STOCHASTIC && dtype_out != LIBXSMM_DATATYPE_BF8) || (rnd_mode > RND_STOCHASTIC)) {
    printf(" Error! rnd_mode = %u is not supported with the selected output precision, prec_out : %i\n", rnd_mode, (int)dtype_out );
    exit(-1);
  }

  if (use_bcast != NO_BCAST) {
    if (use_bcast == ROW_BCAST) {
      printf("Using row broadcast for the input row-vector ...\n");
    }
    if (use_bcast == COL_BCAST) {
      printf("Using column broadcast for the input column-vector...\n");
    }
    if (use_bcast == SCALAR_BCAST) {
      printf("Using scalar broadcast for the input value...\n");
    }
  }

  if (op == REPLICATE_COL_VAR) {
    use_bcast = COL_BCAST;
  }

  if ( (op == COPY_OP) || (op == XOR_OP) || (op == REPLICATE_COL_VAR) ) {
    if ( ( (dtype_in == LIBXSMM_DATATYPE_F64 ) && (dtype_out == LIBXSMM_DATATYPE_F64 ) && (dtype_comp == LIBXSMM_DATATYPE_F64 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_BF16) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F16 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF8 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_BF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF8 ) && (dtype_out == LIBXSMM_DATATYPE_BF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF8 ) && (dtype_out == LIBXSMM_DATATYPE_BF8 ) && (dtype_comp == LIBXSMM_DATATYPE_BF8 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_HF8 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_HF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_HF8 ) && (dtype_out == LIBXSMM_DATATYPE_HF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_HF8 ) && (dtype_out == LIBXSMM_DATATYPE_HF8 ) && (dtype_comp == LIBXSMM_DATATYPE_HF8 ) ) ) {
      printf("Testing in:%s out:%s comp:%s unary copy - M=%i, N=%i, LDI=%i, LDO=%i\n",  libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_out), libxsmm_get_typename(dtype_comp), M, N, ldi, ldo);
      ret = test_unary_op( M, N, ldi, ldo, op, use_bcast, dtype_in, dtype_out, dtype_comp, rnd_mode );
    } else {
      printf("  %s, Op: %i, prec_in: %s, compute_prec: %s, prec_out: %s, rnd_mode : %u\n", argv[0], valid_op, libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_comp), libxsmm_get_typename(dtype_out), rnd_mode);
      printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [rnd_mode : 0/1]\n", argv[0] );
      exit(-1);
    }
  } else if (op == UNZIP) {
    if ( ( (dtype_in == LIBXSMM_DATATYPE_F32 || dtype_in == LIBXSMM_DATATYPE_I32 || dtype_in == LIBXSMM_DATATYPE_U32) && (dtype_out == LIBXSMM_DATATYPE_U16) && (dtype_comp == LIBXSMM_DATATYPE_IMPLICIT ) ) ) {
      printf("Testing in:%s out:%s comp:%s unary %s - M=%i, N=%i, LDI=%i, LDO=%i\n",  libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_out), libxsmm_get_typename(dtype_comp), opname, M, N, ldi, ldo);
      ret = test_unary_op( M, N, ldi, ldo, op, use_bcast, dtype_in, dtype_out, dtype_comp, rnd_mode );
    } else {
      printf("  %s, Op: %i, prec_in: %s, compute_prec: %s, prec_out: %s, rnd_mode : %u\n", argv[0], valid_op, libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_comp), libxsmm_get_typename(dtype_out), rnd_mode);
      printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [rnd_mode : 0/1]\n", argv[0] );
      exit(-1);
    }
  } else if ( (op == FP32_TO_BF16X2) || (op == FP32_TO_BF16X3) ) {
    if ( ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ) {
      printf("Testing in:%s out:%s comp:%s unary %s - M=%i, N=%i, LDI=%i, LDO=%i\n",  libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_out), libxsmm_get_typename(dtype_comp), opname, M, N, ldi, ldo);
      ret = test_unary_op( M, N, ldi, ldo, op, use_bcast, dtype_in, dtype_out, dtype_comp, rnd_mode );
    } else {
      printf("  %s, Op: %i, prec_in: %s, compute_prec: %s, prec_out: %s, rnd_mode : %u\n", argv[0], valid_op, libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_comp), libxsmm_get_typename(dtype_out), rnd_mode);
      printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [rnd_mode : 0/1]\n", argv[0] );
      exit(-1);
    }
  } else if ( valid_op > 0 ) {
    if ( ( (dtype_in == LIBXSMM_DATATYPE_F64 ) && (dtype_out == LIBXSMM_DATATYPE_F64 ) && (dtype_comp == LIBXSMM_DATATYPE_F64 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF16) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_BF16) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F16 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_F16 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF8 ) && (dtype_out == LIBXSMM_DATATYPE_BF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_BF8 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_BF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_HF8 ) && (dtype_out == LIBXSMM_DATATYPE_HF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_HF8 ) && (dtype_out == LIBXSMM_DATATYPE_F32 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ||
         ( (dtype_in == LIBXSMM_DATATYPE_F32 ) && (dtype_out == LIBXSMM_DATATYPE_HF8 ) && (dtype_comp == LIBXSMM_DATATYPE_F32 ) ) ) {
      printf("Testing in:%s out:%s comp:%s unary %s - M=%i, N=%i, LDI=%i, LDO=%i\n",  libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_out), libxsmm_get_typename(dtype_comp), opname, M, N, ldi, ldo);
      ret = test_unary_op( M, N, ldi, ldo, op, use_bcast, dtype_in, dtype_out, dtype_comp, rnd_mode );
    } else {
      printf("  %s, Op: %i, prec_in: %s, compute_prec: %s, prec_out: %s, rnd_mode : %u\n", argv[0], valid_op, libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_comp), libxsmm_get_typename(dtype_out), rnd_mode);
      printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [rnd_mode : 0/1]\n", argv[0] );
      exit(-1);
    }
  } else {
    printf("  %s, Op: %i, prec_in: %s, compute_prec: %s, prec_out: %s, rnd_mode : %u\n", argv[0], valid_op, libxsmm_get_typename(dtype_in), libxsmm_get_typename(dtype_comp), libxsmm_get_typename(dtype_out), rnd_mode);
    printf(" Error! Usage: %s [type] [use_bcast: 0/1/2/3] [prec_in: F32/BF16/F16/BF8/HF8] [prec_comp: F32/BF16/F16/BF8/HF8] [prec_out: F32/BF16/F16/BF8/HF8] [M] [N] [ldi] [ldo] [rnd_mode : 0/1]\n", argv[0] );
    exit(-1);
  }

  ret = (ret == EXIT_SUCCESS) ? libxsmm_return_success_code(is_reference_kernel) : ret;
  return ret;
}
