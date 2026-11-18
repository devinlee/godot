#pragma once
#include <meton_gw3_identity.generated.h>
#if METON_GW3_ROLE != 3
#error "Web game cannot consume another role identity"
#endif
#ifndef METON_GW3_BINDING_DIGEST
#error "Meton GW3 binding identity is required"
#endif
#define METON_WEB_BINDING_DIGEST METON_GW3_BINDING_DIGEST
