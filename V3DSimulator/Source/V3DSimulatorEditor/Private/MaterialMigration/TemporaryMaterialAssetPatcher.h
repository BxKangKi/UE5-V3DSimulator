// Copyright © 2026 BxKangKi. Licensed under the MIT License.
//
// ============================================================================
// TEMPORARY MATERIAL-EDITING EDITOR CODE (ISOLATED FOR ONE-SHOT REMOVAL)
// ============================================================================
// Once you have opened the Unreal Editor once and verified that M_Toon,
// MI_Water, and MI_Ocean have been updated and saved, you can delete this
// entire `MaterialMigration/` folder and remove the marked block in
// `V3DSimulatorEditor.cpp`.
// ============================================================================

#pragma once

#include "CoreMinimal.h"

struct FToolMenuSection;

namespace V3DTempMaterialPatcher
{
    /** Registers the post-engine-init automatic one-time material patch. */
    void Register();

    /** Unregisters all delegates owned by this temporary patcher. */
    void Unregister();

    /** Adds a manual Tools menu trigger to re-run the patch on demand. */
    void RegisterMenuEntry(FToolMenuSection& Section);

    /** Applies all temporary material asset patches (idempotent). */
    void ApplyAll();
}
