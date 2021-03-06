// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Engine/GameViewportClient.h"
#include "OffAxisGameViewportClient.generated.h"

/**
 * 
 */
UCLASS(BlueprintType)
class UOffAxisGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:

	FMatrix GenerateOffAxisMatrix(float _screenWidth, float _screenHeight, FVector _eyeRelativePositon, EStereoscopicPass _PassType);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static FMatrix GenerateOffAxisMatrix(float _screenWidth, float _screenHeight,  FVector _eyeRelativePositon);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
	static void SetOffAxisMatrix(FMatrix OffAxisMatrix);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UpdateEyeRelativePosition(FVector _eyeRelativePosition);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void SetWidth(float _width);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void SetHeight(float _height);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void ToggleOffAxisMethod();

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void PrintCurrentOffAxisVersioN();
	
	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UpdateEyeOffsetForStereo(float _newVal);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UpdateProjectionPlaneOffsetForStereo(float _newVal);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void ResetProjectionPlaneOffsetForStereo(float _newVal = 0.f);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void ResetEyeOffsetForStereo(float _newVal = 3.2000005f);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UpdateTmpVector(FVector _newVal);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UpdateShowDebugMessages(bool _newVal);

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void UseOffAxis(bool _newVal);


	virtual void Draw(FViewport* Viewport, FCanvas* SceneCanvas) override;

	UFUNCTION(BlueprintCallable, Category = "OffAxis")
		static void InitOffAxisProjection();


private:
	FName CurrentBufferVisualizationMode;
	FMatrix		mOffAxisMatrix;
	bool		mOffAxisMatrixSetted = false;
	
	/** Delegate called when the engine finishes drawing a game viewport */
	FSimpleMulticastDelegate EndDrawDelegate;

	/** Delegate called when the engine starts drawing a game viewport */
	FSimpleMulticastDelegate BeginDrawDelegate;

	/** Delegate called when the game viewport is drawn, before drawing the console */
	FSimpleMulticastDelegate DrawnDelegate;
};

