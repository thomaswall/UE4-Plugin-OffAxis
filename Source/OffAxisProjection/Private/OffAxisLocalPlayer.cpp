// Fill out your copyright notice in the Description page of Project Settings.

#include "OffAxisLocalPlayer.h"

#include "Runtime/Engine/Public/UnrealEngine.h"


FSceneView * UOffAxisLocalPlayer::CalcSceneView(FSceneViewFamily * ViewFamily, FVector & OutViewLocation, FRotator & OutViewRotation, FViewport * Viewport, FViewElementDrawer * ViewDrawer, EStereoscopicPass StereoPass)
{
	FSceneView* tmp = Super::CalcSceneView(ViewFamily, OutViewLocation, OutViewRotation, Viewport, ViewDrawer, StereoPass);

	UE_LOG(LogConsoleResponse, Warning, TEXT("OffAxisLocalPlayer"));

	return tmp;
}

FMatrix UOffAxisLocalPlayer::CalculateOffAxisMatrix(FVector _eyeRelativePositon, float _screenWidth, float _screenHeight)
{
	FMatrix result;

	float width = _screenWidth;
	float height = _screenHeight;
	FVector eyePosition = _eyeRelativePositon;

	float l, r, b, t, n, f, nd;

	n = GNearClippingPlane;
	f = 10000.f;

	FMatrix matFlipZ;
	matFlipZ.SetIdentity();


	//FMatrix OffAxisProjectionMatrix;

	if (bOffAxisVersion == 0)
	{
		FVector topLeftCorner(-width / 2.f, -height / 2.f, n);
		FVector bottomRightCorner(width / 2.f, height / 2.f, n);

		FVector eyeToTopLeft = topLeftCorner - _eyeRelativePositon;
		FVector eyeToTopLeftNear = n / eyeToTopLeft.Z * eyeToTopLeft;
		FVector eyeToBottomRight = bottomRightCorner - _eyeRelativePositon;
		FVector eyeToBottomRightNear = eyeToBottomRight / eyeToBottomRight.Z * n;

		l = -eyeToTopLeftNear.X;
		r = -eyeToBottomRightNear.X;
		t = -eyeToBottomRightNear.Y;
		b = -eyeToTopLeftNear.Y;


		//Frustum: l, r, b, t, near, far
		result = FrustumMatrix(l, r, b, t, n, f);


	}
	else
	{
		//this is analog to: http://csc.lsu.edu/~kooima/articles/genperspective/

	//Careful: coordinate system! left-handed, y-up

	//lower left, lower right, upper left, eye pos
		const FVector pa(-width / 2.0f, -height / 2.0f, n);
		const FVector pb(width / 2.0f, -height / 2.0f, n);
		const FVector pc(-width / 2.0f, height / 2.0f, n);
		const FVector pe(eyePosition.X, eyePosition.Y, eyePosition.Z);

		// Compute the screen corner vectors.
		FVector va, vb, vc;
		va = pa - pe;
		vb = pb - pe;
		vc = pc - pe;

		// Compute an orthonormal basis for the screen.
		FVector vr, vu, vn;
		vr = pb - pa;
		vr /= vr.Normalize();
		vu = pc - pa;
		vu /= vu.Normalize();
		vn = FVector::CrossProduct(vr, vu);
		vn /= vn.Normalize();

		// Find the distance from the eye to screen plane.
		float d = -FVector::DotProduct(va, vn);

		nd = n / d;

		// Find the extent of the perpendicular projection.
		l = FVector::DotProduct(vr, va) * nd;
		r = FVector::DotProduct(vr, vb) * nd;
		b = FVector::DotProduct(vu, va) * nd;
		t = FVector::DotProduct(vu, vc) * nd;



		// Load the perpendicular projection.
		FMatrix result = FrustumMatrix(l, r, b, t, n, f);
		//GEngine->AddOnScreenDebugMessage(40, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_ORIG: %s"), *result.ToString()));



		// Rotate the projection to be non-perpendicular. 
		// This is currently unused until the screen is used.
		FMatrix M;
		M.SetIdentity();
		M.M[0][0] = vr.X; M.M[0][1] = vr.Y; M.M[0][2] = vr.Z;
		M.M[1][0] = vu.X; M.M[1][1] = vu.Y; M.M[1][2] = vu.Z;
		M.M[2][0] = vn.X; M.M[2][1] = vn.Y; M.M[2][2] = vn.Z;
		M.M[3][3] = 1.0f;
		result = result * M;

		if (bShowDebugMessages)
		{
			GEngine->AddOnScreenDebugMessage(10, 2, FColor::Red, FString::Printf(TEXT("pa: %s"), *pa.ToString()));
			GEngine->AddOnScreenDebugMessage(20, 2, FColor::Red, FString::Printf(TEXT("pb: %s"), *pb.ToString()));
			GEngine->AddOnScreenDebugMessage(30, 2, FColor::Red, FString::Printf(TEXT("pc: %s"), *pc.ToString()));
			GEngine->AddOnScreenDebugMessage(40, 2, FColor::Red, FString::Printf(TEXT("pe: %s"), *pe.ToString()));
			GEngine->AddOnScreenDebugMessage(50, 2, FColor::Red, FString::Printf(TEXT("vr: %s"), *vu.ToString()));
			GEngine->AddOnScreenDebugMessage(60, 2, FColor::Red, FString::Printf(TEXT("vu: %s"), *vr.ToString()));
			GEngine->AddOnScreenDebugMessage(70, 2, FColor::Red, FString::Printf(TEXT("vn: %s"), *vn.ToString()));
			GEngine->AddOnScreenDebugMessage(80, 4, FColor::Red, FString::Printf(TEXT("Frustum: %f \t %f \t %f \t %f \t %f \t %f \t "), l, r, b, t, n, f));
			GEngine->AddOnScreenDebugMessage(90, 2, FColor::Red, FString::Printf(TEXT("Eye-Screen-Distance: %f"), d));
			GEngine->AddOnScreenDebugMessage(100, 4, FColor::Red, FString::Printf(TEXT("nd: %f"), nd));
		}
	}
	// Move the apex of the frustum to the origin.
	result = FTranslationMatrix(-eyePosition) * result;
	//GEngine->AddOnScreenDebugMessage(41, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_MOV: %s"), *result.ToString()));

	//scales matrix for UE4 and RHI
	result *= 1.0f / result.M[0][0];
	//GEngine->AddOnScreenDebugMessage(42, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_DIV: %s"), *result.ToString()));

	result.M[2][2] = 0.f; //?
	result.M[3][2] = n; //?

	//GEngine->AddOnScreenDebugMessage(49, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_MOD : %s"), *result.ToString()));

	return result;
}


FMatrix UOffAxisLocalPlayer::FrustumMatrix(float left, float right, float bottom, float top, float nearVal, float farVal)
{
	FMatrix Result;
	Result.SetIdentity();
	Result.M[0][0] = (2.0f * nearVal) / (right - left);
	Result.M[1][1] = (2.0f * nearVal) / (top - bottom);
	Result.M[2][0] = (right + left) / (right - left);
	Result.M[2][1] = (top + bottom) / (top - bottom);
	Result.M[2][2] = -(farVal + nearVal) / (farVal - nearVal);
	Result.M[2][3] = -1.0f;
	Result.M[3][2] = -(2.0f * farVal * nearVal) / (farVal - nearVal);
	Result.M[3][3] = 0.0f;

	return Result;
}
