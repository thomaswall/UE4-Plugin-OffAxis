// Fill out your copyright notice in the Description page of Project Settings.

#include "OffAxisGameViewportClient.h"

#include "Engine/Console.h"
#include "GameFramework/HUD.h"
#include "ParticleDefinitions.h"
#include "FXSystem.h"
#include "SubtitleManager.h"
#include "ImageUtils.h"
#include "RenderCore.h"
#include "ColorList.h"
#include "SlateBasics.h"
#include "SceneViewExtension.h"
#include "IHeadMountedDisplay.h"
#include "SVirtualJoystick.h"
#include "SceneViewport.h"
#include "EngineModule.h"
#include "AudioDevice.h"
#include "Sound/SoundWave.h"
#include "Engine/GameInstance.h"
#include "HighResScreenshot.h"
#include "Particles/ParticleSystemComponent.h"
#include "BufferVisualizationData.h"
#include "RendererInterface.h"
#include "GameFramework/InputSettings.h"
#include "Components/LineBatchComponent.h"
#include "Debug/DebugDrawService.h"
#include "Components/BrushComponent.h"
#include "Engine/GameEngine.h"
#include "GameFramework/GameUserSettings.h"
#include "Runtime/Engine/Classes/Engine/UserInterfaceSettings.h"
#include "ContentStreaming.h"
#include "SGameLayerManager.h"
#include "ActorEditorUtils.h"
#include "ComponentRecreateRenderStateContext.h"
#include "Stats2.h"
#include "NameTypes.h"

//added during upgrade to 4.18
#include "IXRTrackingSystem.h"

//added during upgrade to 4.19
#include "DynamicResolutionState.h"
#include "LegacyScreenPercentageDriver.h"

//added during plugin creation
#include "Runtime/Engine/Classes//Engine//Canvas.h"
#include "Runtime/Engine/Public/EngineUtils.h"
#include "Runtime/Engine/Classes/Engine/LocalPlayer.h"
#include "Runtime/Engine/Public/UnrealEngine.h"

#pragma warning (disable : 4459 ) /* declaration of xxx hides global declaration */

#define LOCTEXT_NAMESPACE "GameViewport"

/** This variable allows forcing full screen of the first player controller viewport, even if there are multiple controllers plugged in and no cinematic playing. */
bool GForceFullscreen = false;

/** Whether to visualize the lightmap selected by the Debug Camera. */
extern ENGINE_API bool GShowDebugSelectedLightmap;
/** The currently selected component in the actor. */
extern ENGINE_API UPrimitiveComponent* GDebugSelectedComponent;
/** The lightmap used by the currently selected component, if it's a static mesh component. */
extern ENGINE_API class FLightMap2D* GDebugSelectedLightmap;


static int s_OffAxisVersion = 1; //0 = optimized; 1 = default;
static float s_EyeOffsetVal = 3.2000005f;
static float s_ProjectionPlaneOffset = 0.f;
static FVector s_EyePosition;
static float s_Width = 0.f;
static float s_Height = 0.f;
static float s_ShowDebugMessages = false;
static bool s_bUseoffAxis = true;
/**
* UI Stats
*/
//DECLARE_CYCLE_STAT(TEXT("UI Drawing Time"), STAT_UIDrawingTime, STATGROUP_UI);


static TAutoConsoleVariable<int32> CVarSetBlackBordersEnabled(
	TEXT("r.BlackBorders"),
	0,
	TEXT("To draw black borders around the rendered image\n")
	TEXT("(prevents artifacts from post processing passes that read outside of the image e.g. PostProcessAA)\n")
	TEXT("in pixels, 0:off"),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarScreenshotDelegate(
	TEXT("r.ScreenshotDelegate"),
	1,
	TEXT("ScreenshotDelegates prevent processing of incoming screenshot request and break some features. This allows to disable them.\n")
	TEXT("Ideally we rework the delegate code to not make that needed.\n")
	TEXT(" 0: off\n")
	TEXT(" 1: delegates are on (default)"),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarSecondaryScreenPercentage( // TODO: make it a user settings instead?
	TEXT("r.SecondaryScreenPercentage.GameViewport"),
	0,
	TEXT("Override secondary screen percentage for game viewport.\n")
	TEXT(" 0: Compute secondary screen percentage = 100 / DPIScalefactor automaticaly (default);\n")
	TEXT(" 1: override secondary screen percentage."),
	ECVF_Default);

/**
* Draw debug info on a game scene view.
*/
class FGameViewDrawer : public FViewElementDrawer
{
public:
	/**
	* Draws debug info using the given draw interface.
	*/
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
	{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		// Draw a wireframe sphere around the selected lightmap, if requested.
		if (GShowDebugSelectedLightmap && GDebugSelectedComponent && GDebugSelectedLightmap)
		{
			float Radius = GDebugSelectedComponent->Bounds.SphereRadius;
			int32 Sides = FMath::Clamp<int32>(FMath::TruncToInt(Radius*Radius*4.0f*PI / (80.0f*80.0f)), 8, 200);
			DrawWireSphere(PDI, GDebugSelectedComponent->Bounds.Origin, FColor(255, 130, 0), GDebugSelectedComponent->Bounds.SphereRadius, Sides, SDPG_Foreground);
		}
#endif
	}
};

/** Util to find named canvas in transient package, and create if not found */
static UCanvas* GetCanvasByName(FName CanvasName)
{
	// Cache to avoid FString/FName conversions/compares
	static TMap<FName, UCanvas*> CanvasMap;
	UCanvas** FoundCanvas = CanvasMap.Find(CanvasName);
	if (!FoundCanvas)
	{
		UCanvas* CanvasObject = FindObject<UCanvas>(GetTransientPackage(), *CanvasName.ToString());
		if (!CanvasObject)
		{
			CanvasObject = NewObject<UCanvas>(GetTransientPackage(), CanvasName);
			CanvasObject->AddToRoot();
		}

		CanvasMap.Add(CanvasName, CanvasObject);
		return CanvasObject;
	}

	return *FoundCanvas;
}

static FMatrix FrustumMatrix(float left, float right, float bottom, float top, float nearVal, float farVal)
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

static FMatrix GenerateOffAxisMatrix_Internal(float _screenWidth, float _screenHeight,  FVector _eyeRelativePositon)
{

	FMatrix result;

	float width = _screenWidth;
	float height = _screenHeight;
	FVector eyePosition = _eyeRelativePositon;

	float l, r, b, t, n, f, nd;

	n = GNearClippingPlane;
	f = 4000.f;

	FMatrix matFlipZ;
	matFlipZ.SetIdentity();


	//FMatrix OffAxisProjectionMatrix;

	if (s_OffAxisVersion == 0)
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
		result = FrustumMatrix(l, r, b, t, n, f);

		// Rotate the projection to be non-perpendicular. 
		// This is currently unused until the screen is used.
		FMatrix M;
		M.SetIdentity();
		M.M[0][0] = vr.X; M.M[0][1] = vr.Y; M.M[0][2] = vr.Z;
		M.M[1][0] = vu.X; M.M[1][1] = vu.Y; M.M[1][2] = vu.Z;
		M.M[2][0] = vn.X; M.M[2][1] = vn.Y; M.M[2][2] = vn.Z;
		M.M[3][3] = 1.0f;
		result = result * M;

		if (s_ShowDebugMessages)
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
	result = FTranslationMatrix(-_eyeRelativePositon) * result;
	//GEngine->AddOnScreenDebugMessage(41, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_MOV: %s"), *result.ToString()));

	//scales matrix for UE4 and RHI
	result *= 1.0f / result.M[0][0]; 
	//GEngine->AddOnScreenDebugMessage(42, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_DIV: %s"), *result.ToString()));

	result.M[2][2] = 0.f; //?
	result.M[3][2] = n; //?

	//GEngine->AddOnScreenDebugMessage(49, 2, FColor::Red, FString::Printf(TEXT("FrustumMatrix_MOD : %s"), *result.ToString()));

	return result;
}

FMatrix UOffAxisGameViewportClient::GenerateOffAxisMatrix(float _screenWidth, float _screenHeight, FVector _eyeRelativePositon)
{
	return GenerateOffAxisMatrix_Internal(_screenWidth, _screenHeight, _eyeRelativePositon);
}

FMatrix UOffAxisGameViewportClient::GenerateOffAxisMatrix(float _screenWidth, float _screenHeight, FVector _eyeRelativePositon, EStereoscopicPass _PassType)
{
	FVector tmpeye = _eyeRelativePositon;
	switch (_PassType)
	{
	case eSSP_FULL:
		break;
	case eSSP_LEFT_EYE:
		tmpeye += FVector(s_EyeOffsetVal, 0.f, 0.f);
		break;
	case eSSP_RIGHT_EYE:
		tmpeye -= FVector(s_EyeOffsetVal, 0.f, 0.f);
		break;
	case eSSP_MONOSCOPIC_EYE:
		break;
	default:
		break;
	}


	return GenerateOffAxisMatrix(_screenWidth, _screenHeight, tmpeye);
}

void UOffAxisGameViewportClient::SetOffAxisMatrix(FMatrix OffAxisMatrix)
{
	auto This = Cast<UOffAxisGameViewportClient>(GEngine->GameViewport);

	if (This)
	{
		This->mOffAxisMatrixSetted = true;
		This->mOffAxisMatrix = OffAxisMatrix;
	}
}

void UOffAxisGameViewportClient::UpdateEyeRelativePosition(FVector _eyeRelativePosition)
{
	s_EyePosition = _eyeRelativePosition;
}

void UOffAxisGameViewportClient::SetWidth(float _width)
{
	s_Width = _width; 
}

void UOffAxisGameViewportClient::SetHeight(float _height)
{
	s_Height = _height;
}

void UOffAxisGameViewportClient::ToggleOffAxisMethod()
{
	if (s_OffAxisVersion == 0)
	{
		s_OffAxisVersion = 1;
	}
	else
	{
		s_OffAxisVersion = 0;
	}
	PrintCurrentOffAxisVersioN();
}

void UOffAxisGameViewportClient::PrintCurrentOffAxisVersioN()
{
	UE_LOG(LogConsoleResponse, Warning, TEXT("OffAxisVersion: %s"), (s_OffAxisVersion ? TEXT("Basic") : TEXT("Optimized"))); //if true (==1) -> basic, else opitmized
	GEngine->AddOnScreenDebugMessage(30, 4, FColor::Cyan, FString::Printf(TEXT("OffAxisVersion: %s"), (s_OffAxisVersion ? TEXT("Basic") : TEXT("Optimized"))));
}

void UOffAxisGameViewportClient::UpdateEyeOffsetForStereo(float _newVal)
{
	s_EyeOffsetVal += _newVal;

	GEngine->AddOnScreenDebugMessage(40, 2, FColor::Cyan, FString::Printf(TEXT("EyeDistance: %f"), 2 * s_EyeOffsetVal));
}

void UOffAxisGameViewportClient::UpdateProjectionPlaneOffsetForStereo(float _newVal)
{
	s_ProjectionPlaneOffset += _newVal;
	GEngine->AddOnScreenDebugMessage(50, 2, FColor::Cyan, FString::Printf(TEXT("ProjectionPlaneOffset: %f"), s_ProjectionPlaneOffset));
}

void UOffAxisGameViewportClient::ResetProjectionPlaneOffsetForStereo(float _newVal /*= 0.f*/)
{
	s_ProjectionPlaneOffset = _newVal;
}

void UOffAxisGameViewportClient::ResetEyeOffsetForStereo(float _newVal)
{
	s_EyeOffsetVal = _newVal;
}

void UOffAxisGameViewportClient::UpdateShowDebugMessages(bool _newVal)
{
	s_ShowDebugMessages = _newVal;
}

void UOffAxisGameViewportClient::UseOffAxis(bool _newVal)
{
	s_bUseoffAxis = _newVal;
}

static FMatrix _AdjustProjectionMatrixForRHI(const FMatrix& InProjectionMatrix)
{
	const float GMinClipZ = GNearClippingPlane;
	const float GProjectionSignY = 1.0f;

	FScaleMatrix ClipSpaceFixScale(FVector(1.0f, GProjectionSignY, 1.0f - GMinClipZ));
	FTranslationMatrix ClipSpaceFixTranslate(FVector(0.0f, 0.0f, GMinClipZ));
	return InProjectionMatrix * ClipSpaceFixScale * ClipSpaceFixTranslate;
}

static void UpdateProjectionMatrix(FSceneView* View, FMatrix OffAxisMatrix, EStereoscopicPass _Pass)
{
	FMatrix stereoProjectionMatrix = OffAxisMatrix;

	switch (_Pass)
	{
	case eSSP_FULL:
		break;
	case eSSP_LEFT_EYE:
		stereoProjectionMatrix = FTranslationMatrix(FVector(s_ProjectionPlaneOffset, 0.f, 0.f)) * OffAxisMatrix;
		break;
	case eSSP_RIGHT_EYE:
		stereoProjectionMatrix = FTranslationMatrix(FVector(-s_ProjectionPlaneOffset, 0.f, 0.f)) * OffAxisMatrix;
		break;
	case eSSP_MONOSCOPIC_EYE:
		break;
	default:
		break;
	}
	
	FMatrix axisChanger; //rotates everything to UE4 coordinate system.

	axisChanger.SetIdentity();
	axisChanger.M[0][0] = 0.0f;
	axisChanger.M[1][1] = 0.0f;
	axisChanger.M[2][2] = 0.0f;

	axisChanger.M[0][2] = 1.0f;
	axisChanger.M[1][0] = 1.0f;
	axisChanger.M[2][1] = 1.0f;

	View->ProjectionMatrixUnadjustedForRHI = View->ViewMatrices.GetViewMatrix().Inverse() * axisChanger * stereoProjectionMatrix;

	//////////////////////////////////////////////////////////////////////////
	FMatrix* pInvViewMatrix = (FMatrix*)(&View->ViewMatrices.GetInvViewMatrix());
	*pInvViewMatrix = View->ViewMatrices.GetViewMatrix().Inverse();
	FVector* pPreViewTranslation = (FVector*)(&View->ViewMatrices.GetPreViewTranslation());
	*pPreViewTranslation = -View->ViewMatrices.GetViewOrigin();
	//////////////////////////////////////////////////////////////////////////

	FMatrix* pProjectionMatrix = (FMatrix*)(&View->ViewMatrices.GetProjectionMatrix());
	*pProjectionMatrix = _AdjustProjectionMatrixForRHI(View->ProjectionMatrixUnadjustedForRHI);

	//////////////////////////////////////////////////////////////////////////

	FMatrix TranslatedViewMatrix = FTranslationMatrix(-View->ViewMatrices.GetPreViewTranslation()) * View->ViewMatrices.GetViewMatrix();
	FMatrix* pTranslatedViewProjectionMatrix = (FMatrix*)(&View->ViewMatrices.GetTranslatedViewProjectionMatrix());
	*pTranslatedViewProjectionMatrix = TranslatedViewMatrix * View->ViewMatrices.GetProjectionMatrix();

	FMatrix* pInvTranslatedViewProjectionMatrixx = (FMatrix*)(&View->ViewMatrices.GetInvTranslatedViewProjectionMatrix());
	*pInvTranslatedViewProjectionMatrixx = View->ViewMatrices.GetTranslatedViewProjectionMatrix().Inverse();

	View->ShadowViewMatrices = View->ViewMatrices;

	GetViewFrustumBounds(View->ViewFrustum, View->ViewMatrices.GetViewProjectionMatrix(), false);



	//////////////////////////////////////////////////////////////////////////
}

void UOffAxisGameViewportClient::Draw(FViewport* InViewport, FCanvas* SceneCanvas)
{
	//Valid SceneCanvas is required.  Make this explicit.
	check(SceneCanvas);

	//BeginDrawDelegate.Broadcast();

	const bool bStereoRendering = GEngine->IsStereoscopic3D(InViewport);
	FCanvas* DebugCanvas = InViewport->GetDebugCanvas();

	// Create a temporary canvas if there isn't already one.
	static FName CanvasObjectName(TEXT("CanvasObject"));
	UCanvas* CanvasObject = GetCanvasByName(CanvasObjectName);
	CanvasObject->Canvas = SceneCanvas;

	// Create temp debug canvas object
	FIntPoint DebugCanvasSize = InViewport->GetSizeXY();
	static FName DebugCanvasObjectName(TEXT("DebugCanvasObject"));
	UCanvas* DebugCanvasObject = GetCanvasByName(DebugCanvasObjectName);
	DebugCanvasObject->Init(DebugCanvasSize.X, DebugCanvasSize.Y, NULL, DebugCanvas);

	if (DebugCanvas)
	{
		DebugCanvas->SetScaledToRenderTarget(bStereoRendering);
		DebugCanvas->SetStereoRendering(bStereoRendering);
	}
	if (SceneCanvas)
	{
		SceneCanvas->SetScaledToRenderTarget(bStereoRendering);
		SceneCanvas->SetStereoRendering(bStereoRendering);
	}

	bool bUIDisableWorldRendering = false;
	FGameViewDrawer GameViewDrawer;

	UWorld* MyWorld = GetWorld();

	// create the view family for rendering the world scene to the viewport's render target
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		InViewport,
		MyWorld->Scene,
		EngineShowFlags)
		.SetRealtimeUpdate(true));

	//added in 4.19
#if WITH_EDITOR
	if (GIsEditor)
	{
		// Force enable view family show flag for HighDPI derived's screen percentage.
		ViewFamily.EngineShowFlags.ScreenPercentage = true;
	}
#endif

	//	GatherViewExtensions(InViewport, ViewFamily.ViewExtensions);
	ViewFamily.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(InViewport);


	for (auto ViewExt : ViewFamily.ViewExtensions)
	{
		ViewExt->SetupViewFamily(ViewFamily);
	}

	if (bStereoRendering && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice())
	{
		// Allow HMD to modify screen settings
		GEngine->XRSystem->GetHMDDevice()->UpdateScreenSettings(Viewport);
	}

	ESplitScreenType::Type SplitScreenConfig = GetCurrentSplitscreenConfiguration();
	ViewFamily.ViewMode = EViewModeIndex(ViewModeIndex);//added in 4.19
	EngineShowFlagOverride(ESFIM_Game, (EViewModeIndex)ViewModeIndex, ViewFamily.EngineShowFlags, NAME_None, SplitScreenConfig != ESplitScreenType::None);

	if (ViewFamily.EngineShowFlags.VisualizeBuffer && AllowDebugViewmodes())
	{
		// Process the buffer visualization console command
		FName NewBufferVisualizationMode = NAME_None;
		static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(FBufferVisualizationData::GetVisualizationTargetConsoleCommandName());
		if (ICVar)
		{
			static const FName OverviewName = TEXT("Overview");
			FString ModeNameString = ICVar->GetString();
			FName ModeName = *ModeNameString;
			if (ModeNameString.IsEmpty() || ModeName == OverviewName || ModeName == NAME_None)
			{
				NewBufferVisualizationMode = NAME_None;
			}
			else
			{
				if (GetBufferVisualizationData().GetMaterial(ModeName) == NULL)
				{
					// Mode is out of range, so display a message to the user, and reset the mode back to the previous valid one
					UE_LOG(LogConsoleResponse, Warning, TEXT("Buffer visualization mode '%s' does not exist"), *ModeNameString);
					NewBufferVisualizationMode = CurrentBufferVisualizationMode;
					// todo: cvars are user settings, here the cvar state is used to avoid log spam and to auto correct for the user (likely not what the user wants)
					ICVar->Set(*NewBufferVisualizationMode.GetPlainNameString(), ECVF_SetByCode);
				}
				else
				{
					NewBufferVisualizationMode = ModeName;
				}
			}
		}

		if (NewBufferVisualizationMode != CurrentBufferVisualizationMode)
		{
			CurrentBufferVisualizationMode = NewBufferVisualizationMode;
		}
	}

	TMap<ULocalPlayer*, FSceneView*> PlayerViewMap;

	FAudioDevice* AudioDevice = MyWorld->GetAudioDevice();
	TArray<FSceneView*> Views; //4.19

	for (FLocalPlayerIterator Iterator(GEngine, MyWorld); Iterator; ++Iterator)
	{
		ULocalPlayer* LocalPlayer = *Iterator;
		if (LocalPlayer)
		{
			APlayerController* PlayerController = LocalPlayer->PlayerController;

			//int32 NumViews = bStereoRendering ? 2 : 1;
			const int32 NumViews = bStereoRendering ? ((ViewFamily.IsMonoscopicFarFieldEnabled()) ? 3 : GEngine->StereoRenderingDevice->GetDesiredNumberOfViews(bStereoRendering)) : 1; //4.19

			for (int32 i = 0; i < NumViews; ++i)
			{
				// Calculate the player's view information.
				FVector		ViewLocation;
				FRotator	ViewRotation;

				EStereoscopicPass PassType = !bStereoRendering ? eSSP_FULL : ((i == 0) ? eSSP_LEFT_EYE : eSSP_RIGHT_EYE);
				//EStereoscopicPass PassType = bStereoRendering ? GEngine->StereoRenderingDevice->GetViewPassForIndex(bStereoRendering, i) : eSSP_FULL; //4.19

				FSceneView* View = LocalPlayer->CalcSceneView(&ViewFamily, ViewLocation, ViewRotation, InViewport, &GameViewDrawer, PassType);

				/************************************************************************/
				/* OFF-AXIS-MAGIC                                                       */
				/************************************************************************/
				if (s_bUseoffAxis)
				{
					SetOffAxisMatrix(GenerateOffAxisMatrix(s_Width, s_Height, s_EyePosition, PassType));
					UpdateProjectionMatrix(View, mOffAxisMatrix, PassType);
				}

				/************************************************************************/
				/* OFF-AXIS-MAGIC                                                       */
				/************************************************************************/


				if (View)
				{
					Views.Add(View);

					if (View->Family->EngineShowFlags.Wireframe)
					{
						// Wireframe color is emissive-only, and mesh-modifying materials do not use material substitution, hence...
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
						View->SpecularOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}
					else if (View->Family->EngineShowFlags.OverrideDiffuseAndSpecular)
					{
						View->DiffuseOverrideParameter = FVector4(GEngine->LightingOnlyBrightness.R, GEngine->LightingOnlyBrightness.G, GEngine->LightingOnlyBrightness.B, 0.0f);
						View->SpecularOverrideParameter = FVector4(.1f, .1f, .1f, 0.0f);
					}
					else if (View->Family->EngineShowFlags.ReflectionOverride)
					{
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
						View->SpecularOverrideParameter = FVector4(1, 1, 1, 0.0f);
						View->NormalOverrideParameter = FVector4(0, 0, 1, 0.0f);
						View->RoughnessOverrideParameter = FVector2D(0.0f, 0.0f);
					}

					if (!View->Family->EngineShowFlags.Diffuse)
					{
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}

					if (!View->Family->EngineShowFlags.Specular)
					{
						View->SpecularOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}

					View->CurrentBufferVisualizationMode = CurrentBufferVisualizationMode;

					View->CameraConstrainedViewRect = View->UnscaledViewRect;

					// If this is the primary drawing pass, update things that depend on the view location
					if (i == 0)
					{
						// Save the location of the view.
						LocalPlayer->LastViewLocation = ViewLocation;

						PlayerViewMap.Add(LocalPlayer, View);

						// Update the listener.
						if (AudioDevice != NULL && PlayerController != NULL)
						{
							bool bUpdateListenerPosition = true;

							// If the main audio device is used for multiple PIE viewport clients, we only
							// want to update the main audio device listener position if it is in focus
							if (GEngine)
							{
								FAudioDeviceManager* AudioDeviceManager = GEngine->GetAudioDeviceManager();

								// If there is more than one world referencing the main audio device
								if (AudioDeviceManager->GetNumMainAudioDeviceWorlds() > 1)
								{
									uint32 MainAudioDeviceHandle = GEngine->GetAudioDeviceHandle();
									//if (AudioDevice->DeviceHandle == MainAudioDeviceHandle && bHasAudioFocus)
									if (AudioDevice->DeviceHandle == MainAudioDeviceHandle)
									{
										bUpdateListenerPosition = false;
									}

								}
							}

							if (bUpdateListenerPosition)
							{
								FVector Location;
								FVector ProjFront;
								FVector ProjRight;
								PlayerController->GetAudioListenerPosition(/*out*/ Location, /*out*/ ProjFront, /*out*/ ProjRight);

								FTransform ListenerTransform(FRotationMatrix::MakeFromXY(ProjFront, ProjRight));

								// Allow the HMD to adjust based on the head position of the player, as opposed to the view location
								if (GEngine->XRSystem.IsValid() && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled())
								{
									const FVector Offset = GEngine->XRSystem->GetAudioListenerOffset();
									Location += ListenerTransform.TransformPositionNoScale(Offset);
								}

								ListenerTransform.SetTranslation(Location);
								ListenerTransform.NormalizeRotation();

								uint32 ViewportIndex = PlayerViewMap.Num() - 1;
								AudioDevice->SetListener(MyWorld, ViewportIndex, ListenerTransform, (View->bCameraCut ? 0.f : MyWorld->GetDeltaSeconds()));
							}
						}
						if (PassType == eSSP_LEFT_EYE)
						{
							// Save the size of the left eye view, so we can use it to reinitialize the DebugCanvasObject when rendering the console at the end of this method
							DebugCanvasSize = View->UnscaledViewRect.Size();
						}

					}

					// Add view information for resource streaming.
					const float StreamingScale = 1.f / FMath::Clamp<float>(View->LODDistanceFactor, .2f, 1.f); //4.19
																											   //IStreamingManager::Get().AddViewInformation(View->ViewMatrices.GetViewOrigin(), View->ViewRect.Width(), View->ViewRect.Width() * View->ViewMatrices.GetProjectionMatrix().M[0][0]);
					IStreamingManager::Get().AddViewInformation(View->ViewMatrices.GetViewOrigin(), View->UnscaledViewRect.Width(), View->UnscaledViewRect.Width() * View->ViewMatrices.GetProjectionMatrix().M[0][0], StreamingScale);

					MyWorld->ViewLocationsRenderedLastFrame.Add(View->ViewMatrices.GetViewOrigin());
				}
			}
		}
	}

	FinalizeViews(&ViewFamily, PlayerViewMap);

	// Update level streaming.
	MyWorld->UpdateLevelStreaming();

	// Find largest rectangle bounded by all rendered views.
	uint32 MinX = InViewport->GetSizeXY().X, MinY = InViewport->GetSizeXY().Y, MaxX = 0, MaxY = 0;
	uint32 TotalArea = 0;
	{
		for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
		{
			const FSceneView* View = ViewFamily.Views[ViewIndex];

			FIntRect UpscaledViewRect = View->UnscaledViewRect;

			MinX = FMath::Min<uint32>(UpscaledViewRect.Min.X, MinX);
			MinY = FMath::Min<uint32>(UpscaledViewRect.Min.Y, MinY);
			MaxX = FMath::Max<uint32>(UpscaledViewRect.Max.X, MaxX);
			MaxY = FMath::Max<uint32>(UpscaledViewRect.Max.Y, MaxY);
			TotalArea += FMath::TruncToInt(UpscaledViewRect.Width()) * FMath::TruncToInt(UpscaledViewRect.Height());
		}

		// To draw black borders around the rendered image (prevents artifacts from post processing passes that read outside of the image e.g. PostProcessAA)
		{
			int32 BlackBorders = FMath::Clamp(CVarSetBlackBordersEnabled.GetValueOnGameThread(), 0, 10);

			if (ViewFamily.Views.Num() == 1 && BlackBorders)
			{
				MinX += BlackBorders;
				MinY += BlackBorders;
				MaxX -= BlackBorders;
				MaxY -= BlackBorders;
				TotalArea = (MaxX - MinX) * (MaxY - MinY);
			}
		}
	}

	// If the views don't cover the entire bounding rectangle, clear the entire buffer.
	bool bBufferCleared = false;
	if (ViewFamily.Views.Num() == 0 || TotalArea != (MaxX - MinX)*(MaxY - MinY) || bDisableWorldRendering)
	{
		bool bStereoscopicPass = (ViewFamily.Views.Num() != 0 && ViewFamily.Views[0]->StereoPass != eSSP_FULL);
		//SceneCanvas->DrawTile(0, 0, InViewport->GetSizeXY().X, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);

		if (bDisableWorldRendering || !bStereoscopicPass) // TotalArea computation does not work correctly for stereoscopic views //4.19
		{
			SceneCanvas->Clear(FLinearColor::Transparent);
		}
		bBufferCleared = true;
	}

	// Force screen percentage show flag to be turned off if not supported. //4.19
	if (!ViewFamily.SupportsScreenPercentage())
	{
		ViewFamily.EngineShowFlags.ScreenPercentage = false;
	}


	/////////// 4.19


	// Set up secondary resolution fraction for the view family.
	if (!bStereoRendering && ViewFamily.SupportsScreenPercentage())
	{
		float CustomSecondaruScreenPercentage = CVarSecondaryScreenPercentage.GetValueOnGameThread();

		if (CustomSecondaruScreenPercentage > 0.0)
		{
			// Override secondary resolution fraction with CVar.
			ViewFamily.SecondaryViewFraction = FMath::Min(CustomSecondaruScreenPercentage / 100.0f, 1.0f);
		}
		else
		{
			// Automatically compute secondary resolution fraction from DPI.
			ViewFamily.SecondaryViewFraction = GetDPIDerivedResolutionFraction();
		}

		check(ViewFamily.SecondaryViewFraction > 0.0f);
	}

	checkf(ViewFamily.GetScreenPercentageInterface() == nullptr,
		TEXT("Some code has tried to set up an alien screen percentage driver, that could be wrong if not supported very well by the RHI."));

	// Setup main view family with screen percentage interface by dynamic resolution if screen percentage is supported.
	//
	// Do not allow dynamic resolution to touch the view family if not supported to ensure there is no possibility to ruin
	// game play experience on platforms that does not support it, but have it enabled by mistake.
	if (ViewFamily.EngineShowFlags.ScreenPercentage && GEngine->GetDynamicResolutionState() && GEngine->GetDynamicResolutionState()->IsSupported())
	{
		GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::BeginDynamicResolutionRendering);
		GEngine->GetDynamicResolutionState()->SetupMainViewFamily(ViewFamily);

#if CSV_PROFILER
		float ResolutionFraction = GEngine->GetDynamicResolutionState()->GetResolutionFractionApproximation();
		if (ResolutionFraction >= 0.0f)
		{
			CSV_CUSTOM_STAT_GLOBAL(DynamicResolutionFraction, ResolutionFraction, ECsvCustomStatOp::Set);
		}
#endif
	}

	// If a screen percentage interface was not set by dynamic resolution, then create one matching legacy behavior.
	if (ViewFamily.GetScreenPercentageInterface() == nullptr)
	{
		bool AllowPostProcessSettingsScreenPercentage = false;
		float GlobalResolutionFraction = 1.0f;

		if (ViewFamily.EngineShowFlags.ScreenPercentage)
		{
			// Allow FPostProcessSettings::ScreenPercentage.
			AllowPostProcessSettingsScreenPercentage = true;

			// Get global view fraction set by r.ScreenPercentage.
			GlobalResolutionFraction = FLegacyScreenPercentageDriver::GetCVarResolutionFraction();
		}

		ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
			ViewFamily, GlobalResolutionFraction, AllowPostProcessSettingsScreenPercentage));
	}
	else if (bStereoRendering)
	{
		// Change screen percentage method to raw output when doing dynamic resolution with VR if not using TAA upsample.
		for (FSceneView* View : Views)
		{
			if (View->PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::SpatialUpscale)
			{
				View->PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::RawOutput;
			}
		}
	}

	////////4.19

	// Draw the player views.
	if (!bDisableWorldRendering && !bUIDisableWorldRendering && PlayerViewMap.Num() > 0) //-V560
	{
		GetRendererModule().BeginRenderingViewFamily(SceneCanvas, &ViewFamily);
	}
	else //4.19
	{
		// Make sure RHI resources get flushed if we're not using a renderer
		ENQUEUE_UNIQUE_RENDER_COMMAND(UGameViewportClient_FlushRHIResources,
			{
				FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			});
	}

	// Beyond this point, only UI rendering independent from dynamc resolution.
	GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndDynamicResolutionRendering); //4.19


																									  // Clear areas of the rendertarget (backbuffer) that aren't drawn over by the views.
	if (!bBufferCleared)
	{
		// clear left
		if (MinX > 0)
		{
			SceneCanvas->DrawTile(0, 0, MinX, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
		}
		// clear right
		if (MaxX < (uint32)InViewport->GetSizeXY().X)
		{
			SceneCanvas->DrawTile(MaxX, 0, InViewport->GetSizeXY().X, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
		}
		// clear top
		if (MinY > 0)
		{
			SceneCanvas->DrawTile(MinX, 0, MaxX, MinY, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
		}
		// clear bottom
		if (MaxY < (uint32)InViewport->GetSizeXY().Y)
		{
			SceneCanvas->DrawTile(MinX, MaxY, MaxX, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
		}
	}

	// Remove temporary debug lines.
	if (MyWorld->LineBatcher != nullptr)
	{
		MyWorld->LineBatcher->Flush();
	}

	if (MyWorld->ForegroundLineBatcher != nullptr)
	{
		MyWorld->ForegroundLineBatcher->Flush();
	}

	// Draw FX debug information.
	if (MyWorld->FXSystem)
	{
		MyWorld->FXSystem->DrawDebug(SceneCanvas);
	}

	// Render the UI.
	{
		//SCOPE_CYCLE_COUNTER(STAT_UIDrawingTime);

		// render HUD
		bool bDisplayedSubtitles = false;
		for (FConstPlayerControllerIterator Iterator = MyWorld->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			APlayerController* PlayerController = Iterator->Get();
			if (PlayerController)
			{
				ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(PlayerController->Player);
				if (LocalPlayer)
				{
					FSceneView* View = PlayerViewMap.FindRef(LocalPlayer);
					if (View != NULL)
					{
						// rendering to directly to viewport target
						FVector CanvasOrigin(FMath::TruncToFloat(View->UnscaledViewRect.Min.X), FMath::TruncToInt(View->UnscaledViewRect.Min.Y), 0.f);

						CanvasObject->Init(View->UnscaledViewRect.Width(), View->UnscaledViewRect.Height(), View, SceneCanvas);

						// Set the canvas transform for the player's view rectangle.
						check(SceneCanvas);
						SceneCanvas->PushAbsoluteTransform(FTranslationMatrix(CanvasOrigin));
						CanvasObject->ApplySafeZoneTransform();

						// Render the player's HUD.
						if (PlayerController->MyHUD)
						{
							//SCOPE_CYCLE_COUNTER(STAT_HudTime);

							DebugCanvasObject->SceneView = View;
							PlayerController->MyHUD->SetCanvas(CanvasObject, DebugCanvasObject);

							PlayerController->MyHUD->PostRender();

							// Put these pointers back as if a blueprint breakpoint hits during HUD PostRender they can
							// have been changed
							CanvasObject->Canvas = SceneCanvas;
							DebugCanvasObject->Canvas = DebugCanvas;

							// A side effect of PostRender is that the playercontroller could be destroyed
							if (!PlayerController->IsPendingKill())
							{
								PlayerController->MyHUD->SetCanvas(NULL, NULL);
							}
						}

						if (DebugCanvas != NULL)
						{
							DebugCanvas->PushAbsoluteTransform(FTranslationMatrix(CanvasOrigin));
							UDebugDrawService::Draw(ViewFamily.EngineShowFlags, InViewport, View, DebugCanvas);
							DebugCanvas->PopTransform();
						}

						CanvasObject->PopSafeZoneTransform();
						SceneCanvas->PopTransform();

						// draw subtitles
						if (!bDisplayedSubtitles)
						{
							FVector2D MinPos(0.f, 0.f);
							FVector2D MaxPos(1.f, 1.f);
							GetSubtitleRegion(MinPos, MaxPos);

							const uint32 SizeX = SceneCanvas->GetRenderTarget()->GetSizeXY().X;
							const uint32 SizeY = SceneCanvas->GetRenderTarget()->GetSizeXY().Y;
							FIntRect SubtitleRegion(FMath::TruncToInt(SizeX * MinPos.X), FMath::TruncToInt(SizeY * MinPos.Y), FMath::TruncToInt(SizeX * MaxPos.X), FMath::TruncToInt(SizeY * MaxPos.Y));
							FSubtitleManager::GetSubtitleManager()->DisplaySubtitles(SceneCanvas, SubtitleRegion, MyWorld->GetAudioTimeSeconds());
							bDisplayedSubtitles = true;
						}
					}
				}
			}
		}

		//ensure canvas has been flushed before rendering UI
		SceneCanvas->Flush_GameThread();

		//4.19
		/*
		if (DebugCanvas != NULL)
		{
		DebugCanvas->Flush_GameThread();
		}
		*/

		//DrawnDelegate.Broadcast();

		// Allow the viewport to render additional stuff
		PostRender(DebugCanvasObject);

		// Render the console.
		if (ViewportConsole)
		{
			ViewportConsole->PostRender_Console(DebugCanvasObject);
		}
	}


	// Grab the player camera location and orientation so we can pass that along to the stats drawing code.
	FVector PlayerCameraLocation = FVector::ZeroVector;
	FRotator PlayerCameraRotation = FRotator::ZeroRotator;
	{
		for (FConstPlayerControllerIterator Iterator = MyWorld->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			(*Iterator)->GetPlayerViewPoint(PlayerCameraLocation, PlayerCameraRotation);
		}
	}

	//DrawStatsHUD(MyWorld, InViewport, DebugCanvas, DebugCanvasObject, DebugProperties, PlayerCameraLocation, PlayerCameraRotation);


	if (DebugCanvas)
	{
		// Reset the debug canvas to be full-screen before drawing the console
		// (the debug draw service above has messed with the viewport size to fit it to a single player's subregion)
		DebugCanvasObject->Init(DebugCanvasSize.X, DebugCanvasSize.Y, NULL, DebugCanvas);

		DrawStatsHUD(MyWorld, InViewport, DebugCanvas, DebugCanvasObject, DebugProperties, PlayerCameraLocation, PlayerCameraRotation);

		if (GEngine->IsStereoscopic3D(InViewport))
		{
#if 0 //!UE_BUILD_SHIPPING
			// TODO: replace implementation in OculusHMD with a debug renderer
			if (GEngine->XRSystem.IsValid())
			{
				GEngine->XRSystem->DrawDebug(DebugCanvasObject);
			}
#endif
		}

		// Render the console absolutely last because developer input is was matter the most.
		if (ViewportConsole)
		{
			ViewportConsole->PostRender_Console(DebugCanvasObject);
		}
	}
	//EndDrawDelegate.Broadcast();
}