// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "VRRootComponent.h"
//#include "Runtime/Engine/Private/EnginePrivate.h"
#include "WorldCollision.h"
#include "PhysicsPublic.h"
#include "DrawDebugHelpers.h"
#include "IHeadMountedDisplay.h"
#include "VRCharacter.h"
#include "Algo/Copy.h"

#if WITH_PHYSX
//#include "PhysXSupport.h"
#endif // WITH_PHYSX

#include "Components/PrimitiveComponent.h"

DEFINE_LOG_CATEGORY(LogVRRootComponent);
#define LOCTEXT_NAMESPACE "VRRootComponent"

DECLARE_CYCLE_STAT(TEXT("VRRootMovement"), STAT_VRRootMovement, STATGROUP_VRRootComponent);

typedef TArray<FOverlapInfo, TInlineAllocator<3>> TInlineOverlapInfoArray;

// Helper to see if two components can possibly generate overlaps with each other.
FORCEINLINE_DEBUGGABLE static bool CanComponentsGenerateOverlap(const UPrimitiveComponent* MyComponent, /*const*/ UPrimitiveComponent* OtherComp)
{
	return OtherComp
		&& OtherComp->GetGenerateOverlapEvents()
		&& MyComponent
		&& MyComponent->GetGenerateOverlapEvents()
		&& MyComponent->GetCollisionResponseToComponent(OtherComp) == ECR_Overlap;
}


// Predicate to remove components from overlaps array that can no longer overlap
struct FPredicateFilterCannotOverlap
{
	FPredicateFilterCannotOverlap(const UPrimitiveComponent& OwningComponent)
		: MyComponent(OwningComponent)
	{
	}

	bool operator() (const FOverlapInfo& Info) const
	{
		return !CanComponentsGenerateOverlap(&MyComponent, Info.OverlapInfo.GetComponent());
	}

private:
	const UPrimitiveComponent& MyComponent;
};

static int32 bEnableFastOverlapCheck = 1;

// Returns true if we should check the GetGenerateOverlapEvents() flag when gathering overlaps, otherwise we'll always just do it.
static bool ShouldCheckOverlapFlagToQueueOverlaps(const UPrimitiveComponent& ThisComponent)
{
	const FScopedMovementUpdate* CurrentUpdate = ThisComponent.GetCurrentScopedMovement();
	if (CurrentUpdate)
	{
		return CurrentUpdate->RequiresOverlapsEventFlag();
	}
	// By default we require the GetGenerateOverlapEvents() to queue up overlaps, since we require it to trigger events.
	return true;
}

// LOOKING_FOR_PERF_ISSUES
#define PERF_MOVECOMPONENT_STATS 0

namespace PrimitiveComponentStatics
{
	//static const FText MobilityWarnText = LOCTEXT("InvalidMove", "move");
	static const FName MoveComponentName(TEXT("MoveComponent"));
	static const FName UpdateOverlapsName(TEXT("UpdateOverlaps"));
}

// Predicate to determine if an overlap is with a certain AActor.
struct FPredicateOverlapHasSameActor
{
	FPredicateOverlapHasSameActor(const AActor& Owner)
		: MyOwnerPtr(&Owner)
	{
	}

	bool operator() (const FOverlapInfo& Info)
	{
		// MyOwnerPtr is always valid, so we don't need the IsValid() checks in the WeakObjectPtr comparison operator.
		return MyOwnerPtr.HasSameIndexAndSerialNumber(Info.OverlapInfo.Actor);
	}

private:
	const TWeakObjectPtr<const AActor> MyOwnerPtr;
};

// Predicate to determine if an overlap is *NOT* with a certain AActor.
struct FPredicateOverlapHasDifferentActor
{
	FPredicateOverlapHasDifferentActor(const AActor& Owner)
		: MyOwnerPtr(&Owner)
	{
	}

	bool operator() (const FOverlapInfo& Info)
	{
		// MyOwnerPtr is always valid, so we don't need the IsValid() checks in the WeakObjectPtr comparison operator.
		return !MyOwnerPtr.HasSameIndexAndSerialNumber(Info.OverlapInfo.Actor);
	}

private:
	const TWeakObjectPtr<const AActor> MyOwnerPtr;
};

/*
* Predicate for comparing FOverlapInfos when exact weak object pointer index/serial numbers should match, assuming one is not null and not invalid.
* Compare to operator== for WeakObjectPtr which does both HasSameIndexAndSerialNumber *and* IsValid() checks on both pointers.
*/
struct FFastOverlapInfoCompare
{
	FFastOverlapInfoCompare(const FOverlapInfo& BaseInfo)
		: MyBaseInfo(BaseInfo)
	{
	}

	bool operator() (const FOverlapInfo& Info)
	{
		return MyBaseInfo.OverlapInfo.Component.HasSameIndexAndSerialNumber(Info.OverlapInfo.Component)
			&& MyBaseInfo.GetBodyIndex() == Info.GetBodyIndex();
	}

private:
	const FOverlapInfo& MyBaseInfo;

};


// Helper for finding the index of an FOverlapInfo in an Array using the FFastOverlapInfoCompare predicate, knowing that at least one overlap is valid (non-null).
template<class AllocatorType>
FORCEINLINE_DEBUGGABLE int32 IndexOfOverlapFast(const TArray<FOverlapInfo, AllocatorType>& OverlapArray, const FOverlapInfo& SearchItem)
{
	return OverlapArray.IndexOfByPredicate(FFastOverlapInfoCompare(SearchItem));
}

// Helper for adding an FOverlapInfo uniquely to an Array, using IndexOfOverlapFast and knowing that at least one overlap is valid (non-null).
template<class AllocatorType>
FORCEINLINE_DEBUGGABLE void AddUniqueOverlapFast(TArray<FOverlapInfo, AllocatorType>& OverlapArray, FOverlapInfo& NewOverlap)
{
	if (IndexOfOverlapFast(OverlapArray, NewOverlap) == INDEX_NONE)
	{
		OverlapArray.Add(NewOverlap);
	}
}

template<class AllocatorType>
FORCEINLINE_DEBUGGABLE void AddUniqueOverlapFast(TArray<FOverlapInfo, AllocatorType>& OverlapArray, FOverlapInfo&& NewOverlap)
{
	if (IndexOfOverlapFast(OverlapArray, NewOverlap) == INDEX_NONE)
	{
		OverlapArray.Add(NewOverlap);
	}
}

static void PullBackHit(FHitResult& Hit, const FVector& Start, const FVector& End, const float Dist)
{
	const float DesiredTimeBack = FMath::Clamp(0.1f, 0.1f / Dist, 1.f / Dist) + 0.001f;
	Hit.Time = FMath::Clamp(Hit.Time - DesiredTimeBack, 0.f, 1.f);
}

static bool ShouldIgnoreHitResult(const UWorld* InWorld, bool bAllowSimulatingCollision, FHitResult const& TestHit, FVector const& MovementDirDenormalized, const AActor* MovingActor, EMoveComponentFlags MoveFlags)
{
	if (TestHit.bBlockingHit)
	{
		// VR Pawns need to totally ignore simulating components with movement to prevent sickness
		if (!bAllowSimulatingCollision && TestHit.Component.IsValid() && TestHit.Component->IsSimulatingPhysics())
			return true;

		// check "ignore bases" functionality
		if ((MoveFlags & MOVECOMP_IgnoreBases) && MovingActor)	//we let overlap components go through because their overlap is still needed and will cause beginOverlap/endOverlap events
		{
			// ignore if there's a base relationship between moving actor and hit actor
			AActor const* const HitActor = TestHit.GetActor();
			if (HitActor)
			{
				if (MovingActor->IsBasedOnActor(HitActor) || HitActor->IsBasedOnActor(MovingActor))
				{
					return true;
				}
			}
		}

		// If we started penetrating, we may want to ignore it if we are moving out of penetration.
		// This helps prevent getting stuck in walls.
		static const auto CVarHitDistanceTolerance = IConsoleManager::Get().FindConsoleVariable(TEXT("p.HitDistanceTolerance"));
		if ((TestHit.Distance < CVarHitDistanceTolerance->GetFloat() || TestHit.bStartPenetrating) && !(MoveFlags & MOVECOMP_NeverIgnoreBlockingOverlaps))
		{
			static const auto CVarInitialOverlapTolerance = IConsoleManager::Get().FindConsoleVariable(TEXT("p.InitialOverlapTolerance"));
			const float DotTolerance = CVarInitialOverlapTolerance->GetFloat();

			// Dot product of movement direction against 'exit' direction
			const FVector MovementDir = MovementDirDenormalized.GetSafeNormal();
			const float MoveDot = (TestHit.ImpactNormal | MovementDir);

			const bool bMovingOut = MoveDot > DotTolerance;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

			static const auto CVarShowInitialOverlaps = IConsoleManager::Get().FindConsoleVariable(TEXT("p.ShowInitialOverlaps"));
			if (CVarShowInitialOverlaps->GetInt() != 0)
			{
				UE_LOG(LogVRRootComponent, Log, TEXT("Overlapping %s Dir %s Dot %f Normal %s Depth %f"), *GetNameSafe(TestHit.Component.Get()), *MovementDir.ToString(), MoveDot, *TestHit.ImpactNormal.ToString(), TestHit.PenetrationDepth);
				DrawDebugDirectionalArrow(InWorld, TestHit.TraceStart, TestHit.TraceStart + 30.f * TestHit.ImpactNormal, 5.f, bMovingOut ? FColor(64, 128, 255) : FColor(255, 64, 64), true, 4.f);
				if (TestHit.PenetrationDepth > KINDA_SMALL_NUMBER)
				{
					DrawDebugDirectionalArrow(InWorld, TestHit.TraceStart, TestHit.TraceStart + TestHit.PenetrationDepth * TestHit.Normal, 5.f, FColor(64, 255, 64), true, 4.f);
				}
			}
		//	}
#endif

			// If we are moving out, ignore this result!
			if (bMovingOut)
			{
				return true;
			}
		}
	}

	return false;
}

static bool ShouldIgnoreOverlapResult(const UWorld* World, const AActor* ThisActor, const UPrimitiveComponent& ThisComponent, const AActor* OtherActor, const UPrimitiveComponent& OtherComponent, bool bCheckOverlapFlags)
{
	// Don't overlap with self
	if (&ThisComponent == &OtherComponent)
	{
		return true;
	}

	if (bCheckOverlapFlags)
	{
		// Both components must set GetGenerateOverlapEvents()
		if (!ThisComponent.GetGenerateOverlapEvents() || !OtherComponent.GetGenerateOverlapEvents())
		{
			return true;
		}
	}

	if (!ThisActor || !OtherActor)
	{
		return true;
	}

	if (!World || OtherActor == (AActor*)World->GetWorldSettings() || !OtherActor->IsActorInitialized())
	{
		return true;
	}

	return false;
}


UVRRootComponent::UVRRootComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	this->RelativeScale3D = FVector(1.0f, 1.0f, 1.0f);
	this->RelativeLocation = FVector(0, 0, 0);

	// 2.15f is ((MIN_FLOOR_DIST + MAX_FLOOR_DIST) / 2), same value that walking attempts to retain
	// 1.9f is MIN_FLOOR_DIST, this would not go below ledges when hanging off
	VRCapsuleOffset = FVector(-8.0f, 0.0f, 2.15f /*0.0f*/);

	bCenterCapsuleOnHMD = false;


	ShapeColor = FColor(223, 149, 157, 255);

	CapsuleRadius = 20.0f;
	CapsuleHalfHeight = 96.0f;
	bUseEditorCompositing = true;
	OffsetComponentToWorld = FTransform(FQuat(0.0f,0.0f,0.0f,1.0f), FVector::ZeroVector, FVector(1.0f));
	
	// Fixes a problem where headset stays at 0,0,0
	lastCameraLoc = FVector::ZeroVector;
	lastCameraRot = FRotator::ZeroRotator;
	curCameraRot = FRotator::ZeroRotator;
	curCameraLoc = FVector::ZeroVector;
	StoredCameraRotOffset = FRotator::ZeroRotator;
	TargetPrimitiveComponent = NULL;
	owningVRChar = NULL;
	//VRCameraCollider = NULL;

	bAllowSimulatingCollision = false;
	bUseWalkingCollisionOverride = false;
	WalkingCollisionOverride = ECollisionChannel::ECC_Pawn;

	bCalledUpdateTransform = false;

	CanCharacterStepUpOn = ECB_No;
	//bShouldUpdatePhysicsVolume = true;
//	bCheckAsyncSceneOnMove = false;
	SetCanEverAffectNavigation(false);
	bDynamicObstacle = true;

	//bOffsetByHMD = false;
}

/** Represents a UVRRootComponent to the scene manager. */
class FDrawVRCylinderSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	FDrawVRCylinderSceneProxy(const UVRRootComponent* InComponent)
		: FPrimitiveSceneProxy(InComponent)
		, bDrawOnlyIfSelected(InComponent->bDrawOnlyIfSelected)
		, CapsuleRadius(InComponent->GetScaledCapsuleRadius())
		, CapsuleHalfHeight(InComponent->GetScaledCapsuleHalfHeight())
		, ShapeColor(InComponent->ShapeColor)
		, VRCapsuleOffset(InComponent->VRCapsuleOffset)
		//, OffsetComponentToWorld(InComponent->OffsetComponentToWorld)
		, LocalToWorld(InComponent->OffsetComponentToWorld.ToMatrixWithScale())
	{
		bWillEverBeLit = false;
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_GetDynamicMeshElements_DrawDynamicElements);

		//const FMatrix& LocalToWorld = OffsetComponentToWorld.ToMatrixWithScale();//GetLocalToWorld();
		const int32 CapsuleSides = FMath::Clamp<int32>(CapsuleRadius / 4.f, 16, 64);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{

			if (VisibilityMap & (1 << ViewIndex))
			{
				const FSceneView* View = Views[ViewIndex];
				const FLinearColor DrawCapsuleColor = GetViewSelectionColor(ShapeColor, *View, IsSelected(), IsHovered(), false, IsIndividuallySelected());

				FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);

				// If in editor views, lets offset the capsule upwards so that it views correctly
				if (UseEditorCompositing(View))
				{
					DrawWireCapsule(PDI, LocalToWorld.GetOrigin() + FVector(0.f, 0.f, CapsuleHalfHeight), LocalToWorld.GetScaledAxis(EAxis::X), LocalToWorld.GetScaledAxis(EAxis::Y), LocalToWorld.GetScaledAxis(EAxis::Z), DrawCapsuleColor, CapsuleRadius, CapsuleHalfHeight, CapsuleSides, SDPG_World, 1.25f);
				}
				else
					DrawWireCapsule(PDI, LocalToWorld.GetOrigin(), LocalToWorld.GetScaledAxis(EAxis::X), LocalToWorld.GetScaledAxis(EAxis::Y), LocalToWorld.GetScaledAxis(EAxis::Z), DrawCapsuleColor, CapsuleRadius, CapsuleHalfHeight, CapsuleSides, SDPG_World, 1.25f);
			}
		}
	}

	/** Called on render thread to assign new dynamic data */
	void UpdateTransform_RenderThread(const FTransform &NewTransform, float NewHalfHeight)
	{
		check(IsInRenderingThread());
		LocalToWorld = NewTransform.ToMatrixWithScale();
		//OffsetComponentToWorld = NewTransform;
		CapsuleHalfHeight = NewHalfHeight;
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		const bool bProxyVisible = !bDrawOnlyIfSelected || IsSelected();

		// Should we draw this because collision drawing is enabled, and we have collision
		const bool bShowForCollision = View->Family->EngineShowFlags.Collision && IsCollisionEnabled();

		FPrimitiveViewRelevance Result;
		Result.bDrawRelevance = (IsShown(View) && bProxyVisible) || bShowForCollision;
		Result.bDynamicRelevance = true;
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
		return Result;
	}
	virtual uint32 GetMemoryFootprint(void) const override { return(sizeof(*this) + GetAllocatedSize()); }
	uint32 GetAllocatedSize(void) const { return(FPrimitiveSceneProxy::GetAllocatedSize()); }

private:
	const uint32	bDrawOnlyIfSelected : 1;
	const float		CapsuleRadius;
	float		CapsuleHalfHeight;
	FColor	ShapeColor;
	const FVector VRCapsuleOffset;
	//FTransform OffsetComponentToWorld;
	FMatrix LocalToWorld;
};

FPrimitiveSceneProxy* UVRRootComponent::CreateSceneProxy()
{
	return new FDrawVRCylinderSceneProxy(this);
}

void UVRRootComponent::BeginPlay()
{
	Super::BeginPlay();


	if(AVRBaseCharacter * vrOwner = Cast<AVRBaseCharacter>(this->GetOwner()))
	{ 
		TargetPrimitiveComponent = vrOwner->VRReplicatedCamera;
		owningVRChar = vrOwner;
		//VRCameraCollider = vrOwner->VRCameraCollider;
		return;
	}
	else
	{
		TArray<USceneComponent*> children = this->GetAttachChildren();

		for (int i = 0; i < children.Num(); i++)
		{
			if (children[i]->IsA(UCameraComponent::StaticClass()))
			{
				TargetPrimitiveComponent = children[i];
				owningVRChar = NULL;
				return;
			}
		}
	}

	//VRCameraCollider = NULL;
	TargetPrimitiveComponent = NULL;
	owningVRChar = NULL;
}


void UVRRootComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	UVRBaseCharacterMovementComponent * CharMove = nullptr;

	// Need these for passing physics updates to character movement
	if (ACharacter * OwningCharacter = Cast<ACharacter>(GetOwner()))
	{
		CharMove = Cast<UVRBaseCharacterMovementComponent>(OwningCharacter->GetCharacterMovement());
	}

	if (IsLocallyControlled())
	{
		if (OptionalWaistTrackingParent.IsValid())
		{
			FTransform NewTrans = IVRTrackedParentInterface::Default_GetWaistOrientationAndPosition(OptionalWaistTrackingParent);
			curCameraLoc = NewTrans.GetTranslation();
			curCameraRot = NewTrans.Rotator();
		}
		else if (GEngine->XRSystem.IsValid() && GEngine->XRSystem->IsHeadTrackingAllowed())
		{
			FQuat curRot;
			if (!GEngine->XRSystem->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, curRot, curCameraLoc))
			{
				curCameraLoc = lastCameraLoc;
				curCameraRot = lastCameraRot;
			}
			else
				curCameraRot = curRot.Rotator();
		}
		else if (TargetPrimitiveComponent)
		{
			curCameraRot = TargetPrimitiveComponent->RelativeRotation;
			curCameraLoc = TargetPrimitiveComponent->RelativeLocation;
		}
		else
		{
			curCameraRot = FRotator::ZeroRotator;
			curCameraLoc = FVector::ZeroVector;
		}

		// Store a leveled yaw value here so it is only calculated once
		StoredCameraRotOffset = UVRExpansionFunctionLibrary::GetHMDPureYaw_I(curCameraRot);

		// Can adjust the relative tolerances to remove jitter and some update processing
		if (!curCameraLoc.Equals(lastCameraLoc, 0.01f) || !curCameraRot.Equals(lastCameraRot, 0.01f))
		{
			// Also calculate vector of movement for the movement component
			FVector LastPosition = OffsetComponentToWorld.GetLocation();

			bCalledUpdateTransform = false;

			// If the character movement doesn't exist or is not active/ticking
			if (!CharMove || !CharMove->IsComponentTickEnabled() || !CharMove->IsActive())
			{
				OnUpdateTransform(EUpdateTransformFlags::None, ETeleportType::None);
			}
			else // Let the character movement move the capsule instead
			{
				// Skip physics update, let the movement component handle it instead
				OnUpdateTransform(EUpdateTransformFlags::SkipPhysicsUpdate, ETeleportType::None);
			}

			// Get the correct next transform to use
			/*FTransform NextTransform;
			if (bOffsetByHMD) // Manually generate it, the current isn't correct
			{
				FVector Camdiff = curCameraLoc - lastCameraLoc;
				NextTransform = FTransform(StoredCameraRotOffset.Quaternion(), FVector(Camdiff.X, Camdiff.Y, bCenterCapsuleOnHMD ? curCameraLoc.Z : CapsuleHalfHeight) + StoredCameraRotOffset.RotateVector(VRCapsuleOffset), FVector(1.0f)) * GetComponentTransform();
			}
			else
				NextTransform = OffsetComponentToWorld;*/

			FHitResult OutHit;
			FCollisionQueryParams Params("RelativeMovementSweep", false, GetOwner());
			FCollisionResponseParams ResponseParam;

			InitSweepCollisionParams(Params, ResponseParam);
			Params.bFindInitialOverlaps = true;
			bool bBlockingHit = false;


			if (bUseWalkingCollisionOverride)
			{
				bool bAllowWalkingCollision = false;
				if (CharMove != nullptr)
				{
					if (CharMove->MovementMode == EMovementMode::MOVE_Walking || CharMove->MovementMode == EMovementMode::MOVE_NavWalking)
						bAllowWalkingCollision = true;
				}

				if (bAllowWalkingCollision)
					bBlockingHit = GetWorld()->SweepSingleByChannel(OutHit, LastPosition, OffsetComponentToWorld.GetLocation()/*NextTransform.GetLocation()*/, FQuat::Identity, WalkingCollisionOverride, GetCollisionShape(), Params, ResponseParam);

				if (bBlockingHit && OutHit.Component.IsValid())
				{
					if (CharMove != nullptr && CharMove->bIgnoreSimulatingComponentsInFloorCheck && OutHit.Component->IsSimulatingPhysics())
						bHadRelativeMovement = false;
					else
						bHadRelativeMovement = true;
				}
				else
					bHadRelativeMovement = false;
			}
			else
				bHadRelativeMovement = true;

			if (bHadRelativeMovement)
			{
				DifferenceFromLastFrame = OffsetComponentToWorld.GetLocation() - LastPosition;
				//DifferenceFromLastFrame = (NextTransform.GetLocation() - LastPosition);// .GetSafeNormal2D();
				DifferenceFromLastFrame.X = FMath::RoundToFloat(DifferenceFromLastFrame.X * 100.f) / 100.f;
				DifferenceFromLastFrame.Y = FMath::RoundToFloat(DifferenceFromLastFrame.Y * 100.f) / 100.f;
				DifferenceFromLastFrame.Z = 0.0f; // Reset Z to zero, its not used anyway and this lets me reuse the Z component for capsule half height
			}
			else // Zero it out so we don't process off of the change (multiplayer sends this)
				DifferenceFromLastFrame = FVector::ZeroVector;
		}
		else
		{
			bHadRelativeMovement = false;
			DifferenceFromLastFrame = FVector::ZeroVector;
		}

		lastCameraLoc = curCameraLoc;
		lastCameraRot = curCameraRot;
	}
	else
	{
		if (TargetPrimitiveComponent)
		{
			curCameraRot = TargetPrimitiveComponent->RelativeRotation;
			curCameraLoc = TargetPrimitiveComponent->RelativeLocation;
		}
		else
		{
			curCameraRot = FRotator(0.0f, 0.0f, 0.0f);// = FRotator::ZeroRotator;
			curCameraLoc = FVector(0.0f, 0.0f, 0.0f);//FVector::ZeroVector;
		}

		// Store a leveled yaw value here so it is only calculated once
		StoredCameraRotOffset = UVRExpansionFunctionLibrary::GetHMDPureYaw_I(curCameraRot);

		// Can adjust the relative tolerances to remove jitter and some update processing
		if (!curCameraLoc.Equals(lastCameraLoc, 0.01f) || !curCameraRot.Equals(lastCameraRot, 0.01f))
		{
			bCalledUpdateTransform = false;

			// If the character movement doesn't exist or is not active/ticking
			if (!CharMove || !CharMove->IsActive())
			{
				OnUpdateTransform(EUpdateTransformFlags::None, ETeleportType::None);
				if (bNavigationRelevant && bRegistered)
				{
					UpdateNavigationData();
					PostUpdateNavigationData();
				}
			}
			else // Let the character movement move the capsule instead
			{
				// Skip physics update, let the movement component handle it instead
				OnUpdateTransform(EUpdateTransformFlags::SkipPhysicsUpdate, ETeleportType::None);

				// This is an edge case, need to check if the nav data needs updated client side
				if (this->GetOwner()->Role == ENetRole::ROLE_SimulatedProxy)
				{
					if (bNavigationRelevant && bRegistered)
					{
						UpdateNavigationData();
						PostUpdateNavigationData();
					}
				}
			}


			lastCameraRot = curCameraRot;
			lastCameraLoc = curCameraLoc;
		}
	}

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}


void UVRRootComponent::SendPhysicsTransform(ETeleportType Teleport)
{
	BodyInstance.SetBodyTransform(OffsetComponentToWorld, Teleport);
	BodyInstance.UpdateBodyScale(OffsetComponentToWorld.GetScale3D());
}

// Override this so that the physics representation is in the correct location
void UVRRootComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	GenerateOffsetToWorld();
	// Using the physics flag for all of this anyway, no reason for a custom flag, it handles it fine
	if (!(UpdateTransformFlags & EUpdateTransformFlags::SkipPhysicsUpdate))
	{
		bCalledUpdateTransform = true;

		// Just using the 
		if (this->ShouldRender() && this->SceneProxy)
		{
			/*ENQUEUE_UNIQUE_RENDER_COMMAND_THREEPARAMETER(
				FDrawCylinderTransformUpdate,
				FDrawVRCylinderSceneProxy*, CylinderSceneProxy, (FDrawVRCylinderSceneProxy*)SceneProxy,
				FTransform, OffsetComponentToWorld, OffsetComponentToWorld, float, CapsuleHalfHeight, CapsuleHalfHeight,
				{
					CylinderSceneProxy->UpdateTransform_RenderThread(OffsetComponentToWorld, CapsuleHalfHeight);
				}
			);*/

			FTransform lOffsetComponentToWorld = OffsetComponentToWorld;
			float lCapsuleHalfHeight = CapsuleHalfHeight;
			FDrawVRCylinderSceneProxy* CylinderSceneProxy = (FDrawVRCylinderSceneProxy*)SceneProxy;
			ENQUEUE_RENDER_COMMAND(VRRootComponent_SendNewDebugTransform)(
				[CylinderSceneProxy, lOffsetComponentToWorld, lCapsuleHalfHeight](FRHICommandList& RHICmdList)
			{
				CylinderSceneProxy->UpdateTransform_RenderThread(lOffsetComponentToWorld, lCapsuleHalfHeight);
			});

		}

		// Don't want to call primitives version, and the scenecomponents version does nothing
		//Super::OnUpdateTransform(UpdateTransformFlags, Teleport);

		// Always send new transform to physics
		if (bPhysicsStateCreated)
		{
			//If we update transform of welded bodies directly (i.e. on the actual component) we need to update the shape transforms of the parent.
			//If the parent is updated, any welded shapes are automatically updated so we don't need to do this physx update.
			//If the parent is updated and we are NOT welded, the child still needs to update physx
			const bool bTransformSetDirectly = !(UpdateTransformFlags & EUpdateTransformFlags::PropagateFromParent);
			if (bTransformSetDirectly || !IsWelded())
			{
				SendPhysicsTransform(Teleport);
			}
		}
	}
}

FBoxSphereBounds UVRRootComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FVector BoxPoint = FVector(CapsuleRadius, CapsuleRadius, CapsuleHalfHeight);
	//FRotator CamRotOffset(0.0f, curCameraRot.Yaw, 0.0f);

	//FRotator CamRotOffset = UVRExpansionFunctionLibrary::GetHMDPureYaw(curCameraRot);
	/*if(bOffsetByHMD)
		return FBoxSphereBounds(FVector(0, 0, CapsuleHalfHeight) + StoredCameraRotOffset.RotateVector(VRCapsuleOffset), BoxPoint, BoxPoint.Size()).TransformBy(LocalToWorld);
	else*/
		return FBoxSphereBounds(FVector(curCameraLoc.X, curCameraLoc.Y, CapsuleHalfHeight) + StoredCameraRotOffset.RotateVector(VRCapsuleOffset), BoxPoint, BoxPoint.Size()).TransformBy(LocalToWorld);
		
}

void UVRRootComponent::GetNavigationData(FNavigationRelevantData& Data) const
{
	if (bDynamicObstacle)
	{
		//Data.Modifiers.CreateAreaModifiers(this, AreaClass);
		UBodySetup* BodySetup = ((UPrimitiveComponent*)this)->GetBodySetup();
		if (BodySetup == nullptr)
		{
			return;
		}

		for (int32 Idx = 0; Idx < BodySetup->AggGeom.BoxElems.Num(); Idx++)
		{
			const FKBoxElem& BoxElem = BodySetup->AggGeom.BoxElems[Idx];
			const FBox BoxSize = BoxElem.CalcAABB(FTransform::Identity, 1.0f);

			FAreaNavModifier AreaMod(BoxSize, OffsetComponentToWorld, AreaClass);
			Data.Modifiers.Add(AreaMod);
		}

		for (int32 Idx = 0; Idx < BodySetup->AggGeom.SphylElems.Num(); Idx++)
		{
			const FKSphylElem& SphylElem = BodySetup->AggGeom.SphylElems[Idx];
			const FTransform AreaOffset(FVector(0, 0, -SphylElem.Length));

			FAreaNavModifier AreaMod(SphylElem.Radius, SphylElem.Length * 2.0f, AreaOffset *  OffsetComponentToWorld, AreaClass);
			Data.Modifiers.Add(AreaMod);
		}

		for (int32 Idx = 0; Idx < BodySetup->AggGeom.ConvexElems.Num(); Idx++)
		{
			const FKConvexElem& ConvexElem = BodySetup->AggGeom.ConvexElems[Idx];

			FAreaNavModifier AreaMod(ConvexElem.VertexData, 0, ConvexElem.VertexData.Num(), ENavigationCoordSystem::Unreal, OffsetComponentToWorld, AreaClass);
			Data.Modifiers.Add(AreaMod);
		}

		for (int32 Idx = 0; Idx < BodySetup->AggGeom.SphereElems.Num(); Idx++)
		{
			const FKSphereElem& SphereElem = BodySetup->AggGeom.SphereElems[Idx];
			const FTransform AreaOffset(FVector(0, 0, -SphereElem.Radius));

			FAreaNavModifier AreaMod(SphereElem.Radius, SphereElem.Radius * 2.0f, AreaOffset *  OffsetComponentToWorld, AreaClass);
			Data.Modifiers.Add(AreaMod);
		}
	}
}

#if WITH_EDITOR
void UVRRootComponent::PreEditChange(UProperty* PropertyThatWillChange)
{
	// This is technically not correct at all to do...however when overloading a root component the preedit gets called twice for some reason.
	// Calling it twice attempts to double register it in the list and causes an assert to be thrown.
	if (this->GetOwner()->IsA(AVRCharacter::StaticClass()))
		return;	
	else
		Super::PreEditChange(PropertyThatWillChange);
}

void UVRRootComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	// We only want to modify the property that was changed at this point
	// things like propagation from CDO to instances don't work correctly if changing one property causes a different property to change
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UVRRootComponent, CapsuleHalfHeight))
	{
		CapsuleHalfHeight = FMath::Max3(0.f, CapsuleHalfHeight, CapsuleRadius);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UVRRootComponent, CapsuleRadius))
	{
		CapsuleRadius = FMath::Clamp(CapsuleRadius, 0.f, CapsuleHalfHeight);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UVRRootComponent, VRCapsuleOffset))
	{
	}

	if (!IsTemplate())
	{
		//UpdateBodySetup(); // do this before reregistering components so that new values are used for collision
	}

	return;

	// Overrode the defaults for this, don't call the parent
	//Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif	// WITH_EDITOR


// This overrides the movement logic to use the offset location instead of the default location for sweeps.
bool UVRRootComponent::MoveComponentImpl(const FVector& Delta, const FQuat& NewRotationQuat, bool bSweep, FHitResult* OutHit, EMoveComponentFlags MoveFlags, ETeleportType Teleport)
{
	SCOPE_CYCLE_COUNTER(STAT_VRRootMovement);

	// static things can move before they are registered (e.g. immediately after streaming), but not after.
	if (IsPendingKill() || (this->Mobility == EComponentMobility::Static && IsRegistered()))//|| CheckStaticMobilityAndWarn(PrimitiveComponentStatics::MobilityWarnText))
	{
		if (OutHit)
		{
			OutHit->Init();
		}
		return false;
	}

	ConditionalUpdateComponentToWorld();

	// Init HitResult
	//FHitResult BlockingHit(1.f);
	const FVector TraceStart = OffsetComponentToWorld.GetLocation();// .GetLocation();//GetComponentLocation();
	const FVector TraceEnd = TraceStart + Delta;
	//BlockingHit.TraceStart = TraceStart;
	//BlockingHit.TraceEnd = TraceEnd;
	float DeltaSizeSq = (TraceEnd - TraceStart).SizeSquared();				// Recalc here to account for precision loss of float addition

	// Set up.
//	float DeltaSizeSq = Delta.SizeSquared();
	const FQuat InitialRotationQuat = GetComponentTransform().GetRotation();//ComponentToWorld.GetRotation();

	// ComponentSweepMulti does nothing if moving < KINDA_SMALL_NUMBER in distance, so it's important to not try to sweep distances smaller than that. 
	const float MinMovementDistSq = (bSweep ? FMath::Square(4.f*KINDA_SMALL_NUMBER) : 0.f);
	if (DeltaSizeSq <= MinMovementDistSq)
	{
		// Skip if no vector or rotation.
		if (NewRotationQuat.Equals(InitialRotationQuat, SCENECOMPONENT_QUAT_TOLERANCE))
		{
			// copy to optional output param
			if (OutHit)
			{
				OutHit->Init(TraceStart, TraceEnd);
			}
			return true;
		}
		DeltaSizeSq = 0.f;
	}

	const bool bSkipPhysicsMove = ((MoveFlags & MOVECOMP_SkipPhysicsMove) != MOVECOMP_NoFlags);

	// WARNING: HitResult is only partially initialized in some paths. All data is valid only if bFilledHitResult is true.
	FHitResult BlockingHit(NoInit);
	BlockingHit.bBlockingHit = false;
	BlockingHit.Time = 1.f;
	bool bFilledHitResult = false;
	bool bMoved = false;
	bool bIncludesOverlapsAtEnd = false;
	bool bRotationOnly = false;
	TArray<FOverlapInfo> PendingOverlaps;
	AActor* const Actor = GetOwner();
	FVector OrigLocation = GetComponentLocation();

	if (!bSweep)
	{
		// not sweeping, just go directly to the new transform
		bMoved = InternalSetWorldLocationAndRotation(/*TraceEnd*/OrigLocation + Delta, NewRotationQuat, bSkipPhysicsMove, Teleport);
		GenerateOffsetToWorld();
		bRotationOnly = (DeltaSizeSq == 0);
		bIncludesOverlapsAtEnd = bRotationOnly && (AreSymmetricRotations(InitialRotationQuat, NewRotationQuat, GetComponentScale())) && IsCollisionEnabled();
	}
	else
	{
		TArray<FHitResult> Hits;
		FVector NewLocation = OrigLocation;//TraceStart;
		// Perform movement collision checking if needed for this actor.
		const bool bCollisionEnabled = IsQueryCollisionEnabled();
		if (bCollisionEnabled && (DeltaSizeSq > 0.f))
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			if (!IsRegistered())
			{
				if (Actor)
				{
					ensureMsgf(IsRegistered(), TEXT("%s MovedComponent %s not initialized deleteme %d"),*Actor->GetName(), *GetName(), Actor->IsPendingKill());
				}
				else
				{ //-V523
					ensureMsgf(IsRegistered(), TEXT("MovedComponent %s not initialized"), *GetFullName());
				}
			}
#endif
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST) && PERF_MOVECOMPONENT_STATS
			MoveTimer.bDidLineCheck = true;
#endif 
			UWorld* const MyWorld = GetWorld();

			const bool bForceGatherOverlaps = !ShouldCheckOverlapFlagToQueueOverlaps(*this);
			
			FComponentQueryParams Params(/*PrimitiveComponentStatics::MoveComponentName*//*"MoveComponent"*/SCENE_QUERY_STAT(MoveComponent), Actor);
			FCollisionResponseParams ResponseParam;
			InitSweepCollisionParams(Params, ResponseParam);
			Params.bIgnoreTouches |= !(GetGenerateOverlapEvents() || bForceGatherOverlaps);
			bool const bHadBlockingHit = MyWorld->ComponentSweepMulti(Hits, this, TraceStart, TraceEnd, InitialRotationQuat, Params);
			//bool const bHadBlockingHit = MyWorld->SweepMultiByChannel(Hits, TraceStart, TraceEnd, InitialRotationQuat, this->GetCollisionObjectType(), this->GetCollisionShape(), Params, ResponseParam);

			if (Hits.Num() > 0)
			{
				const float DeltaSize = FMath::Sqrt(DeltaSizeSq);
				for (int32 HitIdx = 0; HitIdx < Hits.Num(); HitIdx++)
				{
					PullBackHit(Hits[HitIdx], TraceStart, TraceEnd, DeltaSize);
				}
			}

			// If we had a valid blocking hit, store it.
			// If we are looking for overlaps, store those as well.
			int32 FirstNonInitialOverlapIdx = INDEX_NONE;
			if (bHadBlockingHit || (GetGenerateOverlapEvents() || bForceGatherOverlaps))
			{
				int32 BlockingHitIndex = INDEX_NONE;
				float BlockingHitNormalDotDelta = BIG_NUMBER;
				for (int32 HitIdx = 0; HitIdx < Hits.Num(); HitIdx++)
				{
					const FHitResult& TestHit = Hits[HitIdx];

					if (TestHit.bBlockingHit)
					{
						if (!ShouldIgnoreHitResult(MyWorld, bAllowSimulatingCollision, TestHit, Delta, Actor, MoveFlags))
						{
							if (TestHit.Time == 0.f)
							{
								// We may have multiple initial hits, and want to choose the one with the normal most opposed to our movement.
								const float NormalDotDelta = (TestHit.ImpactNormal | Delta);
								if (NormalDotDelta < BlockingHitNormalDotDelta)
								{
									BlockingHitNormalDotDelta = NormalDotDelta;
									BlockingHitIndex = HitIdx;
								}
							}
							else if (BlockingHitIndex == INDEX_NONE)
							{
								// First non-overlapping blocking hit should be used, if an overlapping hit was not.
								// This should be the only non-overlapping blocking hit, and last in the results.
								BlockingHitIndex = HitIdx;
								break;
							}
						}
					}
					else if (GetGenerateOverlapEvents() || bForceGatherOverlaps)
					{
						UPrimitiveComponent* OverlapComponent = TestHit.Component.Get();
						if (OverlapComponent && (OverlapComponent->GetGenerateOverlapEvents() || bForceGatherOverlaps))
						{
							if (!ShouldIgnoreOverlapResult(MyWorld, Actor, *this, TestHit.GetActor(), *OverlapComponent,!bForceGatherOverlaps))
							{
								// don't process touch events after initial blocking hits
								if (BlockingHitIndex >= 0 && TestHit.Time > Hits[BlockingHitIndex].Time)
								{
									break;
								}

								if (FirstNonInitialOverlapIdx == INDEX_NONE && TestHit.Time > 0.f)
								{
									// We are about to add the first non-initial overlap.
									FirstNonInitialOverlapIdx = PendingOverlaps.Num();
								}

								// cache touches
								AddUniqueOverlapFast(PendingOverlaps, FOverlapInfo(TestHit));
							}
						}
					}
				}

				// Update blocking hit, if there was a valid one.
				if (BlockingHitIndex >= 0)
				{
					BlockingHit = Hits[BlockingHitIndex];
					bFilledHitResult = true;
				}
			}

			// Update NewLocation based on the hit result
			if (!BlockingHit.bBlockingHit)
			{
				NewLocation += (TraceEnd - TraceStart);
			}
			else
			{
				check(bFilledHitResult);
				NewLocation += (BlockingHit.Time * (TraceEnd - TraceStart));

				// Sanity check
				const FVector ToNewLocation = (NewLocation - OrigLocation/*TraceStart*/);
				if (ToNewLocation.SizeSquared() <= MinMovementDistSq)
				{
					// We don't want really small movements to put us on or inside a surface.
					NewLocation = OrigLocation;//TraceStart;
					BlockingHit.Time = 0.f;

					// Remove any pending overlaps after this point, we are not going as far as we swept.
					if (FirstNonInitialOverlapIdx != INDEX_NONE)
					{
						const bool bAllowShrinking = false;
						PendingOverlaps.SetNum(FirstNonInitialOverlapIdx, bAllowShrinking);
					}
				}
			}

			bIncludesOverlapsAtEnd = AreSymmetricRotations(InitialRotationQuat, NewRotationQuat, GetComponentScale());
		}
		else if (DeltaSizeSq > 0.f)
		{
			// apply move delta even if components has collisions disabled
			NewLocation += Delta;
			bIncludesOverlapsAtEnd = false;
		}
		else if (DeltaSizeSq == 0.f && bCollisionEnabled)
		{
			bIncludesOverlapsAtEnd = AreSymmetricRotations(InitialRotationQuat, NewRotationQuat, GetComponentScale());
			bRotationOnly = true;
		}

		// Update the location.  This will teleport any child components as well (not sweep).
		bMoved = InternalSetWorldLocationAndRotation(NewLocation, NewRotationQuat, bSkipPhysicsMove, Teleport);
		GenerateOffsetToWorld();
	}

	// Handle overlap notifications.
	if (bMoved)
	{
		if (IsDeferringMovementUpdates())
		{
			// Defer UpdateOverlaps until the scoped move ends.
			FScopedMovementUpdate* ScopedUpdate = GetCurrentScopedMovement();
			if (bRotationOnly && bIncludesOverlapsAtEnd)
			{
				ScopedUpdate->KeepCurrentOverlapsAfterRotation(bSweep);
			}
			else
			{
				ScopedUpdate->AppendOverlapsAfterMove(PendingOverlaps, bSweep, bIncludesOverlapsAtEnd);
			}
		}
		else
		{
			if (bIncludesOverlapsAtEnd)
			{
				TArray<FOverlapInfo> OverlapsAtEndLocation;
				const TArray<FOverlapInfo>* OverlapsAtEndLocationPtr = nullptr; // When non-null, used as optimization to avoid work in UpdateOverlaps.
				if (bRotationOnly)
				{
					OverlapsAtEndLocationPtr = ConvertRotationOverlapsToCurrentOverlaps(OverlapsAtEndLocation, GetOverlapInfos());
				}
				else
				{
					OverlapsAtEndLocationPtr = ConvertSweptOverlapsToCurrentOverlaps(OverlapsAtEndLocation, PendingOverlaps, 0, /*GetComponentLocation()*/OffsetComponentToWorld.GetLocation(), GetComponentQuat());
				}

				UpdateOverlaps(&PendingOverlaps, true, OverlapsAtEndLocationPtr);
			}
			else
			{
				UpdateOverlaps(&PendingOverlaps, true, nullptr);
			}
		}
	}

	// Handle blocking hit notifications. Avoid if pending kill (which could happen after overlaps).
	const bool bAllowHitDispatch = !BlockingHit.bStartPenetrating || !(MoveFlags & MOVECOMP_DisableBlockingOverlapDispatch);
	if (BlockingHit.bBlockingHit && bAllowHitDispatch && !IsPendingKill())
	{
		check(bFilledHitResult);
		if (IsDeferringMovementUpdates())
		{
			FScopedMovementUpdate* ScopedUpdate = GetCurrentScopedMovement();
			ScopedUpdate->AppendBlockingHitAfterMove(BlockingHit);
		}
		else
		{
			DispatchBlockingHit(*Actor, BlockingHit);
		}
	}

	// copy to optional output param
	if (OutHit)
	{
		if (bFilledHitResult)
		{
			*OutHit = BlockingHit;
		}
		else
		{
			OutHit->Init(TraceStart, TraceEnd);
		}
	}

	// Return whether we moved at all.
	return bMoved;
}

bool UVRRootComponent::UpdateOverlapsImpl(const TArray<FOverlapInfo>* NewPendingOverlaps, bool bDoNotifies, const TArray<FOverlapInfo>* OverlapsAtEndLocation)
{
	//SCOPE_CYCLE_COUNTER(STAT_UpdateOverlaps);
	bool bCanSkipUpdateOverlaps = true;

	// first, dispatch any pending overlaps
	if (GetGenerateOverlapEvents() && IsQueryCollisionEnabled())	//TODO: should modifying query collision remove from mayoverlapevents?
	{
		bCanSkipUpdateOverlaps = false;

		// if we haven't begun play, we're still setting things up (e.g. we might be inside one of the construction scripts)
		// so we don't want to generate overlaps yet.
		AActor* const MyActor = GetOwner();
		if (MyActor && MyActor->IsActorInitialized())
		{
			const FTransform PrevTransform = GetComponentTransform();
			// If we are the root component we ignore child components. Those children will update their overlaps when we descend into the child tree.
			// This aids an optimization in MoveComponent.
			const bool bIgnoreChildren = (MyActor->GetRootComponent() == this);

			if (NewPendingOverlaps)
			{	 
				// Note: BeginComponentOverlap() only triggers overlaps where GetGenerateOverlapEvents() is true on both components.
				for (int32 Idx = 0; Idx < NewPendingOverlaps->Num(); ++Idx)
				{
					BeginComponentOverlap((*NewPendingOverlaps)[Idx], bDoNotifies);
				}
			}

			const TArray<FOverlapInfo>* OverlapsAtEndLocationPtr;

			// TODO: Filter this better so it runs even less often?
			// Its not that bad currently running off of NewPendingOverlaps
			// It forces checking for end location overlaps again if none are registered, just in case
			// the capsule isn't setting things correctly.
			TArray<FOverlapInfo> OverlapsAtEnd;
			if ((!OverlapsAtEndLocation || OverlapsAtEndLocation->Num() < 1) && NewPendingOverlaps && NewPendingOverlaps->Num() > 0)
			{
				OverlapsAtEndLocationPtr = ConvertSweptOverlapsToCurrentOverlaps(OverlapsAtEnd, *NewPendingOverlaps, 0, OffsetComponentToWorld.GetLocation(), GetComponentQuat());
			}
			else
				OverlapsAtEndLocationPtr = OverlapsAtEndLocation;

			// now generate full list of new touches, so we can compare to existing list and
			// determine what changed
			TInlineOverlapInfoArray NewOverlappingComponents;

			// If pending kill, we should not generate any new overlaps
			if (!IsPendingKill())
			{
				// 4.17 converted to auto cvar
				static const auto CVarAllowCachedOverlaps = IConsoleManager::Get().FindConsoleVariable(TEXT("p.AllowCachedOverlaps"));
				// Might be able to avoid testing for new overlaps at the end location.
				if (OverlapsAtEndLocationPtr != NULL && CVarAllowCachedOverlaps && PrevTransform.Equals(GetComponentTransform()))
				{
					UE_LOG(LogVRRootComponent, VeryVerbose, TEXT("%s->%s Skipping overlap test!"), *GetNameSafe(GetOwner()), *GetName());
					NewOverlappingComponents = *OverlapsAtEndLocationPtr;

					// BeginComponentOverlap may have disabled what we thought were valid overlaps at the end (collision response or overlap flags could change).
					// Or we have overlaps from a scoped update that didn't require overlap events, but we need to remove those now.
					if (NewPendingOverlaps && NewPendingOverlaps->Num() > 0)
					{
						NewOverlappingComponents.RemoveAllSwap(FPredicateFilterCannotOverlap(*this), false);
					}
				}
				else
				{
					UE_LOG(LogVRRootComponent, VeryVerbose, TEXT("%s->%s Performing overlaps!"), *GetNameSafe(GetOwner()), *GetName());
					UWorld* const MyWorld = MyActor->GetWorld();
					TArray<FOverlapResult> Overlaps;
					// note this will optionally include overlaps with components in the same actor (depending on bIgnoreChildren). 

					FComponentQueryParams Params(SCENE_QUERY_STAT(UpdateOverlaps), bIgnoreChildren ? MyActor : nullptr); //(PrimitiveComponentStatics::UpdateOverlapsName, bIgnoreChildren ? MyActor : nullptr);
					
					Params.bIgnoreBlocks = true;	//We don't care about blockers since we only route overlap events to real overlaps
					FCollisionResponseParams ResponseParam;
					InitSweepCollisionParams(Params, ResponseParam);
					ComponentOverlapMulti(Overlaps, MyWorld, OffsetComponentToWorld.GetTranslation(), GetComponentQuat(), GetCollisionObjectType(), Params);

					for (int32 ResultIdx = 0; ResultIdx < Overlaps.Num(); ResultIdx++)
					{
						const FOverlapResult& Result = Overlaps[ResultIdx];

						UPrimitiveComponent* const HitComp = Result.Component.Get();
						if (HitComp && (HitComp != this) && HitComp->GetGenerateOverlapEvents())
						{
							if (!ShouldIgnoreOverlapResult(MyWorld, MyActor, *this, Result.GetActor(), *HitComp,  true))
							{
								NewOverlappingComponents.Add(FOverlapInfo(HitComp, Result.ItemIndex));		// don't need to add unique unless the overlap check can return dupes
							}
						}
					}
				}
			}

			if (OverlappingComponents.Num() > 0)
			{
				// make a copy of the old that we can manipulate to avoid n^2 searching later
				TInlineOverlapInfoArray OldOverlappingComponents;
				if (bIgnoreChildren)
				{
					Algo::CopyIf(OverlappingComponents, OldOverlappingComponents, FPredicateOverlapHasDifferentActor(*MyActor));
				}
				else
				{
					OldOverlappingComponents = OverlappingComponents;
				}

				// Now we want to compare the old and new overlap lists to determine 
				// what overlaps are in old and not in new (need end overlap notifies), and 
				// what overlaps are in new and not in old (need begin overlap notifies).
				// We do this by removing common entries from both lists, since overlapping status has not changed for them.
				// What is left over will be what has changed.
				for (int32 CompIdx = 0; CompIdx < OldOverlappingComponents.Num() && NewOverlappingComponents.Num() > 0; ++CompIdx)
				{
					// RemoveSingleSwap is ok, since it is not necessary to maintain order
					const bool bAllowShrinking = false;


					const FOverlapInfo& SearchItem = OldOverlappingComponents[CompIdx];
					const int32 NewElementIdx = IndexOfOverlapFast(NewOverlappingComponents, SearchItem);
					if (NewElementIdx != INDEX_NONE)
					{
						NewOverlappingComponents.RemoveAtSwap(NewElementIdx, 1, bAllowShrinking);
						OldOverlappingComponents.RemoveAtSwap(CompIdx, 1, bAllowShrinking);
						--CompIdx;
					}
				}

				// OldOverlappingComponents now contains only previous overlaps that are confirmed to no longer be valid.
				for (auto CompIt = OldOverlappingComponents.CreateIterator(); CompIt; ++CompIt)
				{
					const FOverlapInfo& OtherOverlap = *CompIt;
					if (OtherOverlap.OverlapInfo.Component.IsValid())
					{
						EndComponentOverlap(OtherOverlap, bDoNotifies, false);
					}
					else
					{
						// Remove stale item. Reclaim memory only if it's getting large, to try to avoid churn but avoid bloating component's memory usage.
						const bool bAllowShrinking = (OverlappingComponents.Max() >= 24);

						const int32 StaleElementIndex = IndexOfOverlapFast(OverlappingComponents, OtherOverlap);
						if (StaleElementIndex != INDEX_NONE)
						{
							OverlappingComponents.RemoveAtSwap(StaleElementIndex, 1, bAllowShrinking);
						}
					}
				}
			}

			// NewOverlappingComponents now contains only new overlaps that didn't exist previously.
			for (auto CompIt = NewOverlappingComponents.CreateIterator(); CompIt; ++CompIt)
			{
				const FOverlapInfo& OtherOverlap = *CompIt;
				BeginComponentOverlap(OtherOverlap, bDoNotifies);
			}
		}
	}
	else
	{
		// GetGenerateOverlapEvents() is false or collision is disabled
		// End all overlaps that exist, in case GetGenerateOverlapEvents() was true last tick (i.e. was just turned off)
		if (OverlappingComponents.Num() > 0)
		{
			const bool bSkipNotifySelf = false;
			ClearComponentOverlaps(bDoNotifies, bSkipNotifySelf);
		}
	}

	// now update any children down the chain.
	// since on overlap events could manipulate the child array we need to take a copy
	// of it to avoid missing any children if one is removed from the middle
	TInlineComponentArray<USceneComponent*> AttachedChildren;
	AttachedChildren.Append(GetAttachChildren());

	for (USceneComponent* const ChildComp : AttachedChildren)
	{
		if (ChildComp)
		{
			// Do not pass on OverlapsAtEndLocation, it only applied to this component.
			bCanSkipUpdateOverlaps &= ChildComp->UpdateOverlaps(nullptr, bDoNotifies, nullptr);
		}
	}

	// Update physics volume using most current overlaps
	if (GetShouldUpdatePhysicsVolume())
	{
		UpdatePhysicsVolume(bDoNotifies);
		bCanSkipUpdateOverlaps = false;
	}

	return bCanSkipUpdateOverlaps;
}


const TArray<FOverlapInfo>* UVRRootComponent::ConvertSweptOverlapsToCurrentOverlaps(
	TArray<FOverlapInfo>& OverlapsAtEndLocation, const TArray<FOverlapInfo>& SweptOverlaps, int32 SweptOverlapsIndex,
	const FVector& EndLocation, const FQuat& EndRotationQuat)
{
	checkSlow(SweptOverlapsIndex >= 0);

	const TArray<FOverlapInfo>* Result = nullptr;
	const bool bForceGatherOverlaps = !ShouldCheckOverlapFlagToQueueOverlaps(*this);

	static const auto CVarAllowCachedOverlaps = IConsoleManager::Get().FindConsoleVariable(TEXT("p.AllowCachedOverlaps"));
	if ((GetGenerateOverlapEvents() || bForceGatherOverlaps) && CVarAllowCachedOverlaps->GetInt())
	{
		const AActor* Actor = GetOwner();
		if (Actor && Actor->GetRootComponent() == this)
		{
			// We know we are not overlapping any new components at the end location. Children are ignored here (see note below).
			if (bEnableFastOverlapCheck)
			{
				//SCOPE_CYCLE_COUNTER(STAT_MoveComponent_FastOverlap);

				// Check components we hit during the sweep, keep only those still overlapping
				const FCollisionQueryParams UnusedQueryParams(NAME_None, FCollisionQueryParams::GetUnknownStatId());
				for (int32 Index = SweptOverlapsIndex; Index < SweptOverlaps.Num(); ++Index)
				{
					const FOverlapInfo& OtherOverlap = SweptOverlaps[Index];
					UPrimitiveComponent* OtherPrimitive = OtherOverlap.OverlapInfo.GetComponent();
					if (OtherPrimitive && (OtherPrimitive->GetGenerateOverlapEvents() || bForceGatherOverlaps))
					{
						if (OtherPrimitive->bMultiBodyOverlap)
						{
							// Not handled yet. We could do it by checking every body explicitly and track each body index in the overlap test, but this seems like a rare need.
							return nullptr;
						}
						else if (Cast<USkeletalMeshComponent>(OtherPrimitive) || Cast<USkeletalMeshComponent>(this))
						{
							// SkeletalMeshComponent does not support this operation, and would return false in the test when an actual query could return true.
							return nullptr;
						}
						else if (OtherPrimitive->ComponentOverlapComponent(this, EndLocation, EndRotationQuat, UnusedQueryParams))
						{
							OverlapsAtEndLocation.Add(OtherOverlap);
						}
					}
				}

				// Note: we don't worry about adding any child components here, because they are not included in the sweep results.
				// Children test for their own overlaps after we update our own, and we ignore children in our own update.
				checkfSlow(OverlapsAtEndLocation.FindByPredicate(FPredicateOverlapHasSameActor(*Actor)) == nullptr,
					TEXT("Child overlaps should not be included in the SweptOverlaps() array in UPrimitiveComponent::ConvertSweptOverlapsToCurrentOverlaps()."));

				Result = &OverlapsAtEndLocation;
			}
			else
			{
				if (SweptOverlaps.Num() == 0 && AreAllCollideableDescendantsRelative())
				{
					// Add overlaps with components in this actor.
					GetOverlapsWithActor(Actor, OverlapsAtEndLocation);
					Result = &OverlapsAtEndLocation;
				}
			}
		}
	}

	return Result;
}


const TArray<FOverlapInfo>* UVRRootComponent::ConvertRotationOverlapsToCurrentOverlaps(TArray<FOverlapInfo>& OverlapsAtEndLocation, const TArray<FOverlapInfo>& CurrentOverlaps)
{
	const TArray<FOverlapInfo>* Result = nullptr;
	const bool bForceGatherOverlaps = !ShouldCheckOverlapFlagToQueueOverlaps(*this);

	static const auto CVarAllowCachedOverlaps = IConsoleManager::Get().FindConsoleVariable(TEXT("p.AllowCachedOverlaps"));

	if ((GetGenerateOverlapEvents() || bForceGatherOverlaps) && /*bAllowCachedOverlapsCVar*/ CVarAllowCachedOverlaps->GetInt())
	{
		const AActor* Actor = GetOwner();
		if (Actor && Actor->GetRootComponent() == this)
		{
			if (bEnableFastOverlapCheck)
			{
				// Add all current overlaps that are not children. Children test for their own overlaps after we update our own, and we ignore children in our own update.
				Algo::CopyIf(CurrentOverlaps, OverlapsAtEndLocation, FPredicateOverlapHasDifferentActor(*Actor));
				Result = &OverlapsAtEndLocation;
			}
		}
	}

	return Result;
}

bool UVRRootComponent::IsLocallyControlled() const
{
	// I like epics implementation better than my own
	const AActor* MyOwner = GetOwner();
	return MyOwner->HasLocalNetOwner();
	//const APawn* MyPawn = Cast<APawn>(MyOwner);
	//return MyPawn ? MyPawn->IsLocallyControlled() : (MyOwner->Role == ENetRole::ROLE_Authority);
}

void UVRRootComponent::UpdatePhysicsVolume(bool bTriggerNotifiers)
{
	if (GetShouldUpdatePhysicsVolume() && !IsPendingKill())
	{
		//	SCOPE_CYCLE_COUNTER(STAT_UpdatePhysicsVolume);
		if (UWorld * MyWorld = GetWorld())
		{
			if (MyWorld->GetNonDefaultPhysicsVolumeCount() == 0)
			{
				SetPhysicsVolume(MyWorld->GetDefaultPhysicsVolume(), bTriggerNotifiers);
			}
			else if (GetGenerateOverlapEvents() && IsQueryCollisionEnabled())
			{
				APhysicsVolume* BestVolume = MyWorld->GetDefaultPhysicsVolume();
				int32 BestPriority = BestVolume->Priority;

				for (auto CompIt = OverlappingComponents.CreateIterator(); CompIt; ++CompIt)
				{
					const FOverlapInfo& Overlap = *CompIt;
					UPrimitiveComponent* OtherComponent = Overlap.OverlapInfo.Component.Get();
					if (OtherComponent && OtherComponent->GetGenerateOverlapEvents())
					{
						APhysicsVolume* V = Cast<APhysicsVolume>(OtherComponent->GetOwner());
						if (V && V->Priority > BestPriority)
						{
							//if (V->IsOverlapInVolume(*this))
							if (AreWeOverlappingVolume(V))
							{
								BestPriority = V->Priority;
								BestVolume = V;
							}
						}
					}
				}

				SetPhysicsVolume(BestVolume, bTriggerNotifiers);
			}
			else
			{
				Super::UpdatePhysicsVolume(bTriggerNotifiers);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE