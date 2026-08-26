/**************************************************************************/
/*  baked_visibility_backend_metal.h                                      */
/**************************************************************************/

#pragma once

#include "baked_visibility_backend.h"

BakedVisibilityBackendCapabilities baked_visibility_metal_probe();
Error baked_visibility_metal_execute(const BakedVisibilityBackendBatchInput &p_input, BakedVisibilityBackendBatchOutput &r_output, String *r_error = nullptr);
Error baked_visibility_metal_execute_certificates(const BakedVisibilityBackendCertificateBatchInput &p_input, BakedVisibilityBackendCertificateBatchOutput &r_output, String *r_error = nullptr);
