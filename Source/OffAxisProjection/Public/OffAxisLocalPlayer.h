// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "OffAxisLocalPlayer.generated.h"

/**
 * 
 */
UCLASS()
class OFFAXISPROJECTION_API UOffAxisLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	virtual FSceneView* CalcSceneView(class FSceneViewFamily* ViewFamily,
			FVector& OutViewLocation,
			FRotator& OutViewRotation,
			FViewport* Viewport,
			class FViewElementDrawer* ViewDrawer = NULL,
			EStereoscopicPass StereoPass = eSSP_FULL) override;

protected:
	FMatrix CalculateOffAxisMatrix(FVector _eyeRelativePositon, float _screenWidth, float _screenHeight);
	FMatrix FrustumMatrix(float left, float right, float bottom, float top, float nearVal, float farVal);

private:

	bool bShowDebugMessages = false;
	bool bOffAxisVersion = false;
};
