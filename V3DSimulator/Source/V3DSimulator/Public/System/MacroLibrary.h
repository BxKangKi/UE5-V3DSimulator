// Copyright © 2026 BxKangKi. Licensed under the MIT License.
// Copyright © 2026 Epic Games, Inc. All rights reserved.

/**
 * @file MacroLibrary.h
 * Role: Defines this source unit's responsibility within V3DSimulator.
 * Key responsibilities: Implements the behavior exposed by this source unit's public API.
 * Declares interface, lifetime, and data-ownership contracts; see the matching implementation for behavior.
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "System/StringHelper.h"

#define WORLD_MAX_SIZE 2147483647.0f
#define BOX_BUFFER_SIZE 100.0f


#define RAGDOLL TEXT("Ragdoll")

#define BONE_HAIR_ROOT TEXT("hairRoot")
#define BONE_DYN_ROOT TEXT("dynRoot")
#define BONE_ROOT TEXT("Root")
#define BONE_HIPS TEXT("hips")
#define BONE_LEFT_UPPER_LEG TEXT("leftUpperLeg")
#define BONE_RIGHT_UPPER_LEG TEXT("rightUpperLeg")
#define BONE_RIGHT_FOOT TEXT("rightFoot")
#define BONE_LEFT_FOOT TEXT("leftFoot")
#define BONE_HEAD TEXT("head")
