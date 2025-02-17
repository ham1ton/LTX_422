// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "GripMotionControllerComponent.h"
#include "IHeadMountedDisplay.h"
//#include "DestructibleComponent.h" 4.18 moved apex destruct to a plugin
#include "Misc/ScopeLock.h"
#include "Net/UnrealNetwork.h"
#include "PrimitiveSceneInfo.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "IXRSystemAssets.h"
#include "Components/StaticMeshComponent.h"
#include "MotionDelayBuffer.h"
#include "UObject/VRObjectVersion.h"
#include "UObject/UObjectGlobals.h" // for FindObject<>
#include "IXRTrackingSystem.h"
#include "IXRSystemAssets.h"
#include "DrawDebugHelpers.h"
#include "TimerManager.h"
#include "VRBaseCharacter.h"

#include "GripScripts/GS_Default.h"

#include "PhysicsPublic.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConstraintDrives.h"
#include "PhysicsReplication.h"

// Delete this eventually when the physics interface is fixed
#if WITH_PHYSX
#include "PhysXPublic.h"
#endif // WITH_PHYSX

#include "Features/IModularFeatures.h"

DEFINE_LOG_CATEGORY(LogVRMotionController);
//For UE4 Profiler ~ Stat
DECLARE_CYCLE_STAT(TEXT("TickGrip ~ TickingGrip"), STAT_TickGrip, STATGROUP_TickGrip);
DECLARE_CYCLE_STAT(TEXT("GetGripWorldTransform ~ GettingTransform"), STAT_GetGripTransform, STATGROUP_TickGrip);

// MAGIC NUMBERS
// Constraint multipliers for angular, to avoid having to have two sets of stiffness/damping variables
const float ANGULAR_STIFFNESS_MULTIPLIER = 1.5f;
const float ANGULAR_DAMPING_MULTIPLIER = 1.4f;

// Multiplier for the Interactive Hybrid With Physics grip - When not colliding increases stiffness by this value
const float HYBRID_PHYSICS_GRIP_MULTIPLIER = 10.0f;

namespace {
	/** This is to prevent destruction of motion controller components while they are
	in the middle of being accessed by the render thread */
	FCriticalSection CritSect;

} // anonymous namespace

  // CVars
namespace GripMotionControllerCvars
{
	static int32 DrawDebugGripCOM = 0;
	FAutoConsoleVariableRef CVarDrawCOMDebugSpheres(
		TEXT("vr.DrawDebugCenterOfMassForGrips"),
		DrawDebugGripCOM,
		TEXT("When on, will draw debug speheres for physics grips COM.\n")
		TEXT("0: Disable, 1: Enable"),
		ECVF_Default);
}

  //=============================================================================
UGripMotionControllerComponent::UGripMotionControllerComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	PrimaryComponentTick.bTickEvenWhenPaused = true;

	PlayerIndex = 0;
	MotionSource = FXRMotionControllerBase::LeftHandSourceId;
	//Hand = EControllerHand::Left;
	bDisableLowLatencyUpdate = false;
	bHasAuthority = false;
	bUseWithoutTracking = false;
	bAlwaysSendTickGrip = false;
	bAutoActivate = true;

	this->SetIsReplicated(true);

	// Default 100 htz update rate, same as the 100htz update rate of rep_notify, will be capped to 90/45 though because of vsync on HMD
	//bReplicateControllerTransform = true;
	ControllerNetUpdateRate = 100.0f; // 100 htz is default
	ControllerNetUpdateCount = 0.0f;
	bReplicateWithoutTracking = false;
	bLerpingPosition = false;
	bSmoothReplicatedMotion = false;
	bReppedOnce = false;
	bOffsetByHMD = false;
	bIsPostTeleport = false;

	GripIDIncrementer = INVALID_VRGRIP_ID;

	bOffsetByControllerProfile = true;
	GripRenderThreadProfileTransform = FTransform::Identity;
	CurrentControllerProfileTransform = FTransform::Identity;

	DefaultGripScript = nullptr;
	DefaultGripScriptClass = UGS_Default::StaticClass();
}

//=============================================================================
UGripMotionControllerComponent::~UGripMotionControllerComponent()
{
	// Moved view extension destruction to BeginDestroy like the new controllers
	// Epic had it listed as a crash in the private bug tracker I guess.
}

void UGripMotionControllerComponent::NewControllerProfileLoaded()
{
	GetCurrentProfileTransform(false);
}

void UGripMotionControllerComponent::GetCurrentProfileTransform(bool bBindToNoticationDelegate)
{
	if (bOffsetByControllerProfile)
	{
		UVRGlobalSettings* VRSettings = GetMutableDefault<UVRGlobalSettings>();

		if (VRSettings == nullptr)
			return;

		EControllerHand HandType;
		this->GetHandType(HandType);

		FTransform NewControllerProfileTransform = FTransform::Identity;

		if (HandType == EControllerHand::Left || HandType == EControllerHand::AnyHand || !VRSettings->bUseSeperateHandTransforms)
		{
			NewControllerProfileTransform = VRSettings->CurrentControllerProfileTransform;
		}
		else if (HandType == EControllerHand::Right)
		{
			NewControllerProfileTransform = VRSettings->CurrentControllerProfileTransformRight;
		}

		if (bBindToNoticationDelegate && !NewControllerProfileEvent_Handle.IsValid())
		{
			NewControllerProfileEvent_Handle = VRSettings->OnControllerProfileChangedEvent.AddUObject(this, &UGripMotionControllerComponent::NewControllerProfileLoaded);
		}

		if (!NewControllerProfileTransform.Equals(CurrentControllerProfileTransform))
		{
			FTransform OriginalControllerProfileTransform = CurrentControllerProfileTransform;
			CurrentControllerProfileTransform = NewControllerProfileTransform;

			// Auto adjust for FPS testing pawns
			if (!bTracked && bUseWithoutTracking)
			{
				this->SetRelativeTransform(CurrentControllerProfileTransform * (OriginalControllerProfileTransform.Inverse() * this->GetRelativeTransform()));
			}

			OnControllerProfileTransformChanged.Broadcast(CurrentControllerProfileTransform.Inverse() * OriginalControllerProfileTransform, CurrentControllerProfileTransform);
		}
	}
}

void UGripMotionControllerComponent::InitializeComponent()
{
	Super::InitializeComponent();

	if (!DefaultGripScript && DefaultGripScriptClass)
		DefaultGripScript = DefaultGripScriptClass.GetDefaultObject();
	else
		DefaultGripScript = GetMutableDefault<UGS_Default>();
}

void UGripMotionControllerComponent::OnUnregister()
{

	if (NewControllerProfileEvent_Handle.IsValid())
	{
		UVRGlobalSettings* VRSettings = GetMutableDefault<UVRGlobalSettings>();
		if (VRSettings != nullptr)
		{
			VRSettings->OnControllerProfileChangedEvent.Remove(NewControllerProfileEvent_Handle);
			NewControllerProfileEvent_Handle.Reset();
		}
	}

	for (int i = 0; i < GrippedObjects.Num(); i++)
	{
		DestroyPhysicsHandle(GrippedObjects[i]);

		if(HasGripAuthority(GrippedObjects[i]) || IsServer())
			DropObjectByInterface(GrippedObjects[i].GrippedObject);
		//DropObject(GrippedObjects[i].GrippedObject, false);	
	}
	GrippedObjects.Empty();

	for (int i = 0; i < LocallyGrippedObjects.Num(); i++)
	{
		DestroyPhysicsHandle(LocallyGrippedObjects[i]);

		if (HasGripAuthority(LocallyGrippedObjects[i]) || IsServer())
			DropObjectByInterface(LocallyGrippedObjects[i].GrippedObject);
		//DropObject(LocallyGrippedObjects[i].GrippedObject, false);
	}
	LocallyGrippedObjects.Empty();

	for (int i = 0; i < PhysicsGrips.Num(); i++)
	{
		DestroyPhysicsHandle(&PhysicsGrips[i]);
	}
	PhysicsGrips.Empty();

	// Clear any timers that we are managing
	if (UWorld * myWorld = GetWorld())
	{
		myWorld->GetTimerManager().ClearAllTimersForObject(this);
	}

	ObjectsWaitingForSocketUpdate.Empty();

	Super::OnUnregister();
}

void UGripMotionControllerComponent::BeginDestroy()
{
	Super::BeginDestroy();

	if (GripViewExtension.IsValid())
	{
		{
			// This component could be getting accessed from the render thread so it needs to wait
			// before clearing MotionControllerComponent and allowing the destructor to continue
			FScopeLock ScopeLock(&CritSect);
			GripViewExtension->MotionControllerComponent = NULL;
		}

		GripViewExtension.Reset();
	}
}

void UGripMotionControllerComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UGripMotionControllerComponent::CreateRenderState_Concurrent()
{
	Super::CreateRenderState_Concurrent();
	/*GripRenderThreadRelativeTransform = GetRelativeTransform();
	GripRenderThreadComponentScale = GetComponentScale();*/
	GripRenderThreadProfileTransform = CurrentControllerProfileTransform;
}

void UGripMotionControllerComponent::SendRenderTransform_Concurrent()
{
	GripRenderThreadRelativeTransform = GetRelativeTransform();
	GripRenderThreadComponentScale = GetComponentScale();
	GripRenderThreadProfileTransform = CurrentControllerProfileTransform;

	Super::SendRenderTransform_Concurrent();
}

FBPActorPhysicsHandleInformation * UGripMotionControllerComponent::GetPhysicsGrip(const FBPActorGripInformation & GripInfo)
{
	return PhysicsGrips.FindByKey(GripInfo);
}


bool UGripMotionControllerComponent::GetPhysicsGripIndex(const FBPActorGripInformation & GripInfo, int & index)
{
	index = PhysicsGrips.IndexOfByKey(GripInfo);
	return index != INDEX_NONE;
}

FBPActorPhysicsHandleInformation * UGripMotionControllerComponent::CreatePhysicsGrip(const FBPActorGripInformation & GripInfo)
{
	FBPActorPhysicsHandleInformation * HandleInfo = PhysicsGrips.FindByKey(GripInfo);

	if (HandleInfo)
	{
		DestroyPhysicsHandle(HandleInfo);
		return HandleInfo;
	}

	FBPActorPhysicsHandleInformation NewInfo;
	NewInfo.HandledObject = GripInfo.GrippedObject;
	NewInfo.GripID = GripInfo.GripID;

	int index = PhysicsGrips.Add(NewInfo);

	return &PhysicsGrips[index];
}


//=============================================================================
void UGripMotionControllerComponent::GetLifetimeReplicatedProps(TArray< class FLifetimeProperty > & OutLifetimeProps) const
{
	 Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Skipping the owner with this as the owner will use the controllers location directly
	DOREPLIFETIME_CONDITION(UGripMotionControllerComponent, ReplicatedControllerTransform, COND_SkipOwner);
	DOREPLIFETIME(UGripMotionControllerComponent, GrippedObjects);
	DOREPLIFETIME(UGripMotionControllerComponent, ControllerNetUpdateRate);

	DOREPLIFETIME_CONDITION(UGripMotionControllerComponent, LocallyGrippedObjects, COND_SkipOwner);
//	DOREPLIFETIME(UGripMotionControllerComponent, bReplicateControllerTransform);
}

void UGripMotionControllerComponent::PreReplication(IRepChangedPropertyTracker & ChangedPropertyTracker)
{
	Super::PreReplication(ChangedPropertyTracker);

	// Don't ever replicate these, they are getting replaced by my custom send anyway
	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeLocation, false);
	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeRotation, false);
	DOREPLIFETIME_ACTIVE_OVERRIDE(USceneComponent, RelativeScale3D, false);
}

void UGripMotionControllerComponent::Server_SendControllerTransform_Implementation(FBPVRComponentPosRep NewTransform)
{
	// Store new transform and trigger OnRep_Function
	ReplicatedControllerTransform = NewTransform;

	// Server should no longer call this RPC itself, but if is using non tracked then it will so keeping auth check
	if(!bHasAuthority)
		OnRep_ReplicatedControllerTransform();
}

bool UGripMotionControllerComponent::Server_SendControllerTransform_Validate(FBPVRComponentPosRep NewTransform)
{
	return true;
	// Optionally check to make sure that player is inside of their bounds and deny it if they aren't?
}

void UGripMotionControllerComponent::FGripViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (!MotionControllerComponent)
	{
		return;
	}

	// Set up the late update state for the controller component
	LateUpdate.Setup(MotionControllerComponent->CalcNewComponentToWorld(FTransform()), MotionControllerComponent, false);
}

void UGripMotionControllerComponent::GetPhysicsVelocities(const FBPActorGripInformation &Grip, FVector &AngularVelocity, FVector &LinearVelocity)
{
	UPrimitiveComponent * primComp = Grip.GetGrippedComponent();//Grip.Component;
	AActor * pActor = Grip.GetGrippedActor();

	if (!primComp && pActor)
		primComp = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

	if (!primComp)
	{
		AngularVelocity = FVector::ZeroVector;
		LinearVelocity = FVector::ZeroVector;
		return;
	}

	AngularVelocity = primComp->GetPhysicsAngularVelocityInDegrees();
	LinearVelocity = primComp->GetPhysicsLinearVelocity();
}

void UGripMotionControllerComponent::GetGripByActor(FBPActorGripInformation &Grip, AActor * ActorToLookForGrip, EBPVRResultSwitch &Result)
{
	if (!ActorToLookForGrip)
	{
		Result = EBPVRResultSwitch::OnFailed;
		return;
	}

	FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(ActorToLookForGrip);
	if(!GripInfo)
		GripInfo = LocallyGrippedObjects.FindByKey(ActorToLookForGrip);
	
	if (GripInfo)
	{
		Grip = *GripInfo;// GrippedObjects[i];
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::GetGripByComponent(FBPActorGripInformation &Grip, UPrimitiveComponent * ComponentToLookForGrip, EBPVRResultSwitch &Result)
{
	if (!ComponentToLookForGrip)
	{
		Result = EBPVRResultSwitch::OnFailed;
		return;
	}

	FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(ComponentToLookForGrip);
	if(!GripInfo)
		GripInfo = LocallyGrippedObjects.FindByKey(ComponentToLookForGrip);

	if (GripInfo)
	{
		Grip = *GripInfo;// GrippedObjects[i];
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::GetGripByObject(FBPActorGripInformation &Grip, UObject * ObjectToLookForGrip, EBPVRResultSwitch &Result)
{
	if (!ObjectToLookForGrip)
	{
		Result = EBPVRResultSwitch::OnFailed;
		return;
	}

	FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(ObjectToLookForGrip);
	if(!GripInfo)
		GripInfo = LocallyGrippedObjects.FindByKey(ObjectToLookForGrip);

	if (GripInfo)
	{
		Grip = *GripInfo;// GrippedObjects[i];
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::GetGripByID(FBPActorGripInformation &Grip, uint8 IDToLookForGrip, EBPVRResultSwitch &Result)
{
	if (IDToLookForGrip == INVALID_VRGRIP_ID)
	{
		Result = EBPVRResultSwitch::OnFailed;
		return;
	}

	FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(IDToLookForGrip);
	if (!GripInfo)
		GripInfo = LocallyGrippedObjects.FindByKey(IDToLookForGrip);

	if (GripInfo)
	{
		Grip = *GripInfo;// GrippedObjects[i];
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetGripPaused(const FBPActorGripInformation &Grip, EBPVRResultSwitch &Result, bool bIsPaused, bool bNoConstraintWhenPaused)
{
	int fIndex = GrippedObjects.Find(Grip);

	FBPActorGripInformation * GripInformation = nullptr;

	if (fIndex != INDEX_NONE)
	{
		GripInformation = &GrippedObjects[fIndex];
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			GripInformation = &LocallyGrippedObjects[fIndex];
		}
	}

	if (GripInformation != nullptr)
	{
		if (bNoConstraintWhenPaused)
		{
			if (bIsPaused)
			{
				if (FBPActorPhysicsHandleInformation * PhysHandle = GetPhysicsGrip(*GripInformation))
				{
					DestroyPhysicsHandle(*GripInformation);
				}
			}
			else
			{
				ReCreateGrip(*GripInformation);
			}
		}

		GripInformation->bIsPaused = bIsPaused;
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetPausedTransform(const FBPActorGripInformation &Grip, const FTransform & PausedTransform, bool bTeleport)
{

	FBPActorGripInformation * GripInformation = nullptr;

	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GripInformation = &GrippedObjects[fIndex];
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			GripInformation = &LocallyGrippedObjects[fIndex];
		}
	}
	
	if (GripInformation != nullptr && GripInformation->GrippedObject != nullptr)
	{
		if (bTeleport)
		{
			FTransform ProxyTrans = PausedTransform;
			TeleportMoveGrip_Impl(*GripInformation, true, true, ProxyTrans);
		}
		else
		{
			if (FBPActorPhysicsHandleInformation * PhysHandle = GetPhysicsGrip(GrippedObjects[fIndex]))
			{
				UpdatePhysicsHandleTransform(*GripInformation, PausedTransform);
			}
			else
			{
				if (GripInformation->GripTargetType == EGripTargetType::ActorGrip)
				{
					GripInformation->GetGrippedActor()->SetActorTransform(PausedTransform);
				}
				else
				{
					GripInformation->GetGrippedComponent()->SetWorldTransform(PausedTransform);
				}
			}
		}
	}
}




void UGripMotionControllerComponent::SetGripCollisionType(const FBPActorGripInformation &Grip, EBPVRResultSwitch &Result, EGripCollisionType NewGripCollisionType)
{
	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GrippedObjects[fIndex].GripCollisionType = NewGripCollisionType;
		ReCreateGrip(GrippedObjects[fIndex]);
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			LocallyGrippedObjects[fIndex].GripCollisionType = NewGripCollisionType;

			if (GetNetMode() == ENetMode::NM_Client && !IsTornOff() && LocallyGrippedObjects[fIndex].GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
				Server_NotifyLocalGripAddedOrChanged(LocallyGrippedObjects[fIndex]);

			ReCreateGrip(LocallyGrippedObjects[fIndex]);

			Result = EBPVRResultSwitch::OnSucceeded;
			return;
		}
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetGripLateUpdateSetting(const FBPActorGripInformation &Grip, EBPVRResultSwitch &Result, EGripLateUpdateSettings NewGripLateUpdateSetting)
{
	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GrippedObjects[fIndex].GripLateUpdateSetting = NewGripLateUpdateSetting;
		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			LocallyGrippedObjects[fIndex].GripLateUpdateSetting = NewGripLateUpdateSetting;

			if (GetNetMode() == ENetMode::NM_Client && !IsTornOff() && LocallyGrippedObjects[fIndex].GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
				Server_NotifyLocalGripAddedOrChanged(LocallyGrippedObjects[fIndex]);

			Result = EBPVRResultSwitch::OnSucceeded;
			return;
		}
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetGripRelativeTransform(
	const FBPActorGripInformation &Grip,
	EBPVRResultSwitch &Result,
	const FTransform & NewRelativeTransform
	)
{
	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GrippedObjects[fIndex].RelativeTransform = NewRelativeTransform;
		if (FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(Grip))
		{
			UpdatePhysicsHandle(Grip.GripID);
		}

		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			LocallyGrippedObjects[fIndex].RelativeTransform = NewRelativeTransform;
			if (FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(Grip))
			{
				UpdatePhysicsHandle(Grip.GripID);
			}

			if (GetNetMode() == ENetMode::NM_Client && !IsTornOff() && LocallyGrippedObjects[fIndex].GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
				Server_NotifyLocalGripAddedOrChanged(LocallyGrippedObjects[fIndex]);

			Result = EBPVRResultSwitch::OnSucceeded;
			return;
		}
	}

	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetGripAdditionTransform(
	const FBPActorGripInformation &Grip,
	EBPVRResultSwitch &Result,
	const FTransform & NewAdditionTransform, bool bMakeGripRelative
	)
{
	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GrippedObjects[fIndex].AdditionTransform = CreateGripRelativeAdditionTransform(Grip, NewAdditionTransform, bMakeGripRelative);

		Result = EBPVRResultSwitch::OnSucceeded;
		return;
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			LocallyGrippedObjects[fIndex].AdditionTransform = CreateGripRelativeAdditionTransform(Grip, NewAdditionTransform, bMakeGripRelative);

			Result = EBPVRResultSwitch::OnSucceeded;
			return;
		}
	}
	Result = EBPVRResultSwitch::OnFailed;
}

void UGripMotionControllerComponent::SetGripStiffnessAndDamping(
	const FBPActorGripInformation &Grip,
	EBPVRResultSwitch &Result,
	float NewStiffness, float NewDamping, bool bAlsoSetAngularValues, float OptionalAngularStiffness, float OptionalAngularDamping
	)
{
	Result = EBPVRResultSwitch::OnFailed;
	int fIndex = GrippedObjects.Find(Grip);

	if (fIndex != INDEX_NONE)
	{
		GrippedObjects[fIndex].Stiffness = NewStiffness;
		GrippedObjects[fIndex].Damping = NewDamping;

		if (bAlsoSetAngularValues)
		{
			GrippedObjects[fIndex].AdvancedGripSettings.PhysicsSettings.AngularStiffness = OptionalAngularStiffness;
			GrippedObjects[fIndex].AdvancedGripSettings.PhysicsSettings.AngularDamping = OptionalAngularDamping;
		}

		Result = EBPVRResultSwitch::OnSucceeded;
		SetGripConstraintStiffnessAndDamping(&GrippedObjects[fIndex]);
		//return;
	}
	else
	{
		fIndex = LocallyGrippedObjects.Find(Grip);

		if (fIndex != INDEX_NONE)
		{
			LocallyGrippedObjects[fIndex].Stiffness = NewStiffness;
			LocallyGrippedObjects[fIndex].Damping = NewDamping;

			if (bAlsoSetAngularValues)
			{
				LocallyGrippedObjects[fIndex].AdvancedGripSettings.PhysicsSettings.AngularStiffness = OptionalAngularStiffness;
				LocallyGrippedObjects[fIndex].AdvancedGripSettings.PhysicsSettings.AngularDamping = OptionalAngularDamping;
			}

			if (GetNetMode() == ENetMode::NM_Client && !IsTornOff() && LocallyGrippedObjects[fIndex].GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
				Server_NotifyLocalGripAddedOrChanged(LocallyGrippedObjects[fIndex]);

			Result = EBPVRResultSwitch::OnSucceeded;
			SetGripConstraintStiffnessAndDamping(&LocallyGrippedObjects[fIndex]);
		//	return;
		}
	}
}

FTransform UGripMotionControllerComponent::CreateGripRelativeAdditionTransform_BP(
	const FBPActorGripInformation &GripToSample,
	const FTransform & AdditionTransform,
	bool bGripRelative
)
{
	return CreateGripRelativeAdditionTransform(GripToSample, AdditionTransform, bGripRelative);
}

bool UGripMotionControllerComponent::GripObject(
	UObject * ObjectToGrip,
	const FTransform &WorldOffset,
	bool bWorldOffsetIsRelative,
	FName OptionalSnapToSocketName,
	FName OptionalBoneToGripName,
	EGripCollisionType GripCollisionType,
	EGripLateUpdateSettings GripLateUpdateSetting,
	EGripMovementReplicationSettings GripMovementReplicationSetting,
	float GripStiffness,
	float GripDamping,
	bool bIsSlotGrip)
{
	if (UPrimitiveComponent * PrimComp = Cast<UPrimitiveComponent>(ObjectToGrip))
	{
		return GripComponent(PrimComp, WorldOffset, bWorldOffsetIsRelative, OptionalSnapToSocketName, OptionalBoneToGripName, GripCollisionType,GripLateUpdateSetting,GripMovementReplicationSetting,GripStiffness,GripDamping, bIsSlotGrip);
	}
	else if (AActor * Actor = Cast<AActor>(ObjectToGrip))
	{
		return GripActor(Actor, WorldOffset, bWorldOffsetIsRelative, OptionalSnapToSocketName, OptionalBoneToGripName, GripCollisionType, GripLateUpdateSetting, GripMovementReplicationSetting, GripStiffness, GripDamping, bIsSlotGrip);
	}

	return false;
}

bool UGripMotionControllerComponent::DropObject(
	UObject * ObjectToDrop,
	uint8 GripIDToDrop,
	bool bSimulate,
	FVector OptionalAngularVelocity,
	FVector OptionalLinearVelocity)
{

	if (ObjectToDrop != nullptr)
	{
		FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(ObjectToDrop);
		if (!GripInfo)
			GripInfo = LocallyGrippedObjects.FindByKey(ObjectToDrop);

		if (GripInfo != nullptr)
		{
			return DropGrip(*GripInfo, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);
		}
	}
	else if (GripIDToDrop != INVALID_VRGRIP_ID)
	{
		FBPActorGripInformation * GripInfo = GrippedObjects.FindByKey(GripIDToDrop);
		if (!GripInfo)
			GripInfo = LocallyGrippedObjects.FindByKey(GripIDToDrop);

		if (GripInfo != nullptr)
		{
			return DropGrip(*GripInfo, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);
		}
	}

	return false;
}

bool UGripMotionControllerComponent::GripObjectByInterface(UObject * ObjectToGrip, const FTransform &WorldOffset, bool bWorldOffsetIsRelative, FName OptionalBoneToGripName, bool bIsSlotGrip)
{
	if (UPrimitiveComponent * PrimComp = Cast<UPrimitiveComponent>(ObjectToGrip))
	{
		AActor * Owner = PrimComp->GetOwner();

		if (!Owner)
			return false;

		if (PrimComp->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			EGripCollisionType CollisionType = IVRGripInterface::Execute_GetPrimaryGripType(PrimComp, bIsSlotGrip);

			float Stiffness;
			float Damping;
			IVRGripInterface::Execute_GetGripStiffnessAndDamping(PrimComp, Stiffness, Damping);

			return GripComponent(PrimComp, WorldOffset, bWorldOffsetIsRelative, NAME_None,
				OptionalBoneToGripName,
				CollisionType,
				IVRGripInterface::Execute_GripLateUpdateSetting(PrimComp),
				IVRGripInterface::Execute_GripMovementReplicationType(PrimComp),
				Stiffness,
				Damping,
				bIsSlotGrip
				);
		}
		else if (Owner->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			EGripCollisionType CollisionType = IVRGripInterface::Execute_GetPrimaryGripType(Owner, bIsSlotGrip);

			float Stiffness;
			float Damping;
			IVRGripInterface::Execute_GetGripStiffnessAndDamping(Owner, Stiffness, Damping);

			return GripComponent(PrimComp, WorldOffset, bWorldOffsetIsRelative, NAME_None,
				OptionalBoneToGripName,
				CollisionType,
				IVRGripInterface::Execute_GripLateUpdateSetting(Owner),
				IVRGripInterface::Execute_GripMovementReplicationType(Owner),
				Stiffness,
				Damping,
				bIsSlotGrip
				);
		}
		else
		{
			// No interface, no grip
			return false;
		}
	}
	else if (AActor * Actor = Cast<AActor>(ObjectToGrip))
	{
		UPrimitiveComponent * root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());

		if (!root)
			return false;

		if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			EGripCollisionType CollisionType = IVRGripInterface::Execute_GetPrimaryGripType(root, bIsSlotGrip);

			float Stiffness;
			float Damping;
			IVRGripInterface::Execute_GetGripStiffnessAndDamping(root, Stiffness, Damping);

			return GripActor(Actor, WorldOffset, bWorldOffsetIsRelative, NAME_None,
				OptionalBoneToGripName,
				CollisionType,
				IVRGripInterface::Execute_GripLateUpdateSetting(root),
				IVRGripInterface::Execute_GripMovementReplicationType(root),
				Stiffness,
				Damping,
				bIsSlotGrip
				);
		}
		else if (Actor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			EGripCollisionType CollisionType = IVRGripInterface::Execute_GetPrimaryGripType(Actor, bIsSlotGrip);

			float Stiffness;
			float Damping;
			IVRGripInterface::Execute_GetGripStiffnessAndDamping(Actor, Stiffness, Damping);

			return GripActor(Actor, WorldOffset, bWorldOffsetIsRelative, NAME_None,
				OptionalBoneToGripName,
				CollisionType,
				IVRGripInterface::Execute_GripLateUpdateSetting(Actor),
				IVRGripInterface::Execute_GripMovementReplicationType(Actor),
				Stiffness,
				Damping,
				bIsSlotGrip
				);
		}
		else
		{
			// No interface, no grip
			return false;
		}
	}

	return false;
}

bool UGripMotionControllerComponent::DropObjectByInterface(UObject * ObjectToDrop, uint8 GripIDToDrop, FVector OptionalAngularVelocity, FVector OptionalLinearVelocity)
{
	FBPActorGripInformation * GripInfo = nullptr;
	if (ObjectToDrop != nullptr)
	{
		GripInfo = GrippedObjects.FindByKey(ObjectToDrop);
		if (!GripInfo)
			GripInfo = LocallyGrippedObjects.FindByKey(ObjectToDrop);
	}
	else if (GripIDToDrop != INVALID_VRGRIP_ID)
	{
		GripInfo = GrippedObjects.FindByKey(GripIDToDrop);
		if (!GripInfo)
			GripInfo = LocallyGrippedObjects.FindByKey(GripIDToDrop);
	}

	if (GripInfo == nullptr)
	{
		return false;
	}

	if (UPrimitiveComponent * PrimComp = Cast<UPrimitiveComponent>(GripInfo->GrippedObject))
	{
		AActor * Owner = PrimComp->GetOwner();

		if (!Owner)
			return false;

		if (PrimComp->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			return DropGrip(*GripInfo, IVRGripInterface::Execute_SimulateOnDrop(PrimComp), OptionalAngularVelocity, OptionalLinearVelocity);
			//return DropComponent(PrimComp, IVRGripInterface::Execute_SimulateOnDrop(PrimComp), OptionalAngularVelocity, OptionalLinearVelocity);
		}
		else if (Owner->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			return DropGrip(*GripInfo, IVRGripInterface::Execute_SimulateOnDrop(Owner), OptionalAngularVelocity, OptionalLinearVelocity);
			//return DropComponent(PrimComp, IVRGripInterface::Execute_SimulateOnDrop(Owner), OptionalAngularVelocity, OptionalLinearVelocity);
		}
		else
		{
			// Allowing for failsafe dropping here.
			return DropGrip(*GripInfo, true, OptionalAngularVelocity, OptionalLinearVelocity);
			//return DropComponent(PrimComp, true, OptionalAngularVelocity, OptionalLinearVelocity);
		}
	}
	else if (AActor * Actor = Cast<AActor>(GripInfo->GrippedObject))
	{
		UPrimitiveComponent * root = Cast<UPrimitiveComponent>(Actor->GetRootComponent());

		if (!root)
			return false;

		if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			return DropGrip(*GripInfo, IVRGripInterface::Execute_SimulateOnDrop(root), OptionalAngularVelocity, OptionalLinearVelocity);
		}
		else if (Actor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			return DropGrip(*GripInfo, IVRGripInterface::Execute_SimulateOnDrop(Actor), OptionalAngularVelocity, OptionalLinearVelocity);
		}
		else
		{
			// Failsafe drop here
			return DropGrip(*GripInfo, true, OptionalAngularVelocity, OptionalLinearVelocity);
		}
	}

	return false;
}

bool UGripMotionControllerComponent::GripActor(
	AActor* ActorToGrip, 
	const FTransform &WorldOffset, 
	bool bWorldOffsetIsRelative,
	FName OptionalSnapToSocketName, 
	FName OptionalBoneToGripName,
	EGripCollisionType GripCollisionType, 
	EGripLateUpdateSettings GripLateUpdateSetting,
	EGripMovementReplicationSettings GripMovementReplicationSetting,
	float GripStiffness, 
	float GripDamping,
	bool bIsSlotGrip)
{
	bool bIsLocalGrip = (GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive || GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep);

	if (!IsServer() && !bIsLocalGrip)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was called on the client side as a replicated grip"));
		return false;
	}

	if (!ActorToGrip)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was passed an invalid actor"));
		return false;
	}

	if (GetIsObjectHeld(ActorToGrip))
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was passed an already gripped actor"));
		return false;
	}

	UPrimitiveComponent *root = Cast<UPrimitiveComponent>(ActorToGrip->GetRootComponent());

	if (!root)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController tried to grip an actor without a UPrimitiveComponent Root"));
		return false; // Need a primitive root
	}

	// Has to be movable to work
	if (root->Mobility != EComponentMobility::Movable && (GripCollisionType != EGripCollisionType::CustomGrip && GripCollisionType != EGripCollisionType::EventsOnly))
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController tried to grip an actor set to static mobility not with a Custom Grip"));
		return false; // It is not movable, can't grip it
	}

	FBPAdvGripSettings AdvancedGripSettings;
	UObject * ObjectToCheck = NULL; // Used if having to calculate the transform
	//bool bIgnoreHandRotation = false;

	TArray<FBPGripPair> HoldingControllers;
	bool bIsHeld;
	bool bHadOriginalSettings = false;
	bool bOriginalGravity = false;
	bool bOriginalReplication = false;

	if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
	{
		if(IVRGripInterface::Execute_DenyGripping(root))
			return false; // Interface is saying not to grip it right now

		IVRGripInterface::Execute_IsHeld(root, HoldingControllers, bIsHeld);
		bool bAllowMultipleGrips = IVRGripInterface::Execute_AllowsMultipleGrips(root);
		if (bIsHeld && !bAllowMultipleGrips)
		{
			return false; // Can't multiple grip this object
		}
		else if (bIsHeld)
		{
			// If we are held by multiple controllers then lets copy our original values from the first one	
			if (HoldingControllers[0].HoldingController != nullptr)
			{
				FBPActorGripInformation gripInfo;
				EBPVRResultSwitch result;
				HoldingControllers[0].HoldingController->GetGripByID(gripInfo, HoldingControllers[0].GripID, result);

				if (result != EBPVRResultSwitch::OnFailed)
				{
					bHadOriginalSettings = true;
					bOriginalGravity = gripInfo.bOriginalGravity;
					bOriginalReplication = gripInfo.bOriginalReplicatesMovement;
				}
			}
		}

		AdvancedGripSettings = IVRGripInterface::Execute_AdvancedGripSettings(root);
		ObjectToCheck = root;
	}
	else if (ActorToGrip->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
	{
		if(IVRGripInterface::Execute_DenyGripping(ActorToGrip))
			return false; // Interface is saying not to grip it right now

		IVRGripInterface::Execute_IsHeld(ActorToGrip, HoldingControllers, bIsHeld);
		bool bAllowMultipleGrips = IVRGripInterface::Execute_AllowsMultipleGrips(ActorToGrip);
		if (bIsHeld && !bAllowMultipleGrips)
		{
			return false; // Can't multiple grip this object
		}
		else if (bIsHeld)
		{
			// If we are held by multiple controllers then lets copy our original values from the first one	
			if (HoldingControllers[0].HoldingController != nullptr)
			{
				FBPActorGripInformation gripInfo;
				EBPVRResultSwitch result;
				HoldingControllers[0].HoldingController->GetGripByID(gripInfo, HoldingControllers[0].GripID, result);

				if (result != EBPVRResultSwitch::OnFailed)
				{
					bHadOriginalSettings = true;
					bOriginalGravity = gripInfo.bOriginalGravity;
					bOriginalReplication = gripInfo.bOriginalReplicatesMovement;
				}
			}
		}

		AdvancedGripSettings = IVRGripInterface::Execute_AdvancedGripSettings(ActorToGrip);
		ObjectToCheck = ActorToGrip;
	}

	// So that events caused by sweep and the like will trigger correctly
	ActorToGrip->AddTickPrerequisiteComponent(this);

	FBPActorGripInformation newActorGrip;
	newActorGrip.GripID = GetNextGripID(bIsLocalGrip);
	newActorGrip.GripCollisionType = GripCollisionType;
	newActorGrip.GrippedObject = ActorToGrip;
	if (bHadOriginalSettings)
	{
		newActorGrip.bOriginalReplicatesMovement = bOriginalReplication;
		newActorGrip.bOriginalGravity = bOriginalGravity;
	}
	else
	{
		newActorGrip.bOriginalReplicatesMovement = ActorToGrip->bReplicateMovement;
		newActorGrip.bOriginalGravity = root->IsGravityEnabled();
	}
	newActorGrip.Stiffness = GripStiffness;
	newActorGrip.Damping = GripDamping;
	newActorGrip.AdvancedGripSettings = AdvancedGripSettings;
	newActorGrip.ValueCache.bWasInitiallyRepped = true; // Set this true on authority side so we can skip a function call on tick
	newActorGrip.bIsSlotGrip = bIsSlotGrip;
	newActorGrip.GrippedBoneName = OptionalBoneToGripName;

	// Ignore late update setting if it doesn't make sense with the grip
	switch(newActorGrip.GripCollisionType)
	{
	case EGripCollisionType::ManipulationGrip:
	case EGripCollisionType::ManipulationGripWithWristTwist:
	{
		newActorGrip.GripLateUpdateSetting = EGripLateUpdateSettings::LateUpdatesAlwaysOff; // Late updates are bad for this grip
	}break;

	default:
	{
		newActorGrip.GripLateUpdateSetting = GripLateUpdateSetting;
	}break;
	}

	if (GripMovementReplicationSetting == EGripMovementReplicationSettings::KeepOriginalMovement)
	{
		if (ActorToGrip->bReplicateMovement)
		{
			newActorGrip.GripMovementReplicationSetting = EGripMovementReplicationSettings::ForceServerSideMovement;
		}
		else
		{
			newActorGrip.GripMovementReplicationSetting = EGripMovementReplicationSettings::ForceClientSideMovement;
		}
	}
	else
		newActorGrip.GripMovementReplicationSetting = GripMovementReplicationSetting;

	newActorGrip.GripTargetType = EGripTargetType::ActorGrip;

	if (OptionalSnapToSocketName.IsValid() && root->DoesSocketExist(OptionalSnapToSocketName))
	{
		// I inverse it so that laying out the sockets makes sense
		FTransform sockTrans = root->GetSocketTransform(OptionalSnapToSocketName, ERelativeTransformSpace::RTS_Component);
		sockTrans.SetScale3D(FVector(1.f) / root->GetComponentScale()); // Prep this so that the inverse works correctly
		newActorGrip.RelativeTransform = sockTrans.Inverse();
		newActorGrip.bIsSlotGrip = true; // Set this to a slot grip

		ObjectToCheck = NULL; // Null it back out, socketed grips don't use this
	}
	else if (bWorldOffsetIsRelative)
	{
		if (CustomPivotComponent.IsValid() && !bIsSlotGrip)
		{
			newActorGrip.RelativeTransform = (WorldOffset * this->GetComponentTransform()).GetRelativeTransform(CustomPivotComponent->GetComponentTransform());
		}
		else
		{
			newActorGrip.RelativeTransform = WorldOffset;
		}
	}
	else
	{
		newActorGrip.RelativeTransform = WorldOffset.GetRelativeTransform(GetPivotTransform());
	}

	if (!bIsLocalGrip)
	{
		int32 Index = GrippedObjects.Add(newActorGrip);
		NotifyGrip(newActorGrip);
	}
	else
	{
		int32 Index = LocallyGrippedObjects.Add(newActorGrip);

		if(GetNetMode() == ENetMode::NM_Client && !IsTornOff() && newActorGrip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
			Server_NotifyLocalGripAddedOrChanged(newActorGrip);
		
		NotifyGrip(newActorGrip);
	}

	return true;
}

bool UGripMotionControllerComponent::DropActor(AActor* ActorToDrop, bool bSimulate, FVector OptionalAngularVelocity, FVector OptionalLinearVelocity)
{
	if (!ActorToDrop)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was passed an invalid actor"));
		return false;
	}

	FBPActorGripInformation * GripToDrop = LocallyGrippedObjects.FindByKey(ActorToDrop);

	if(GripToDrop)
		return DropGrip(*GripToDrop, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);

	if (!IsServer())
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was called on the client side with a replicated grip"));
		return false;
	}

	GripToDrop = GrippedObjects.FindByKey(ActorToDrop);
	if (GripToDrop)
		return DropGrip(*GripToDrop, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);

	return false;
}

bool UGripMotionControllerComponent::GripComponent(
	UPrimitiveComponent* ComponentToGrip, 
	const FTransform &WorldOffset, 
	bool bWorldOffsetIsRelative, 
	FName OptionalSnapToSocketName, 
	FName OptionalBoneToGripName,
	EGripCollisionType GripCollisionType,
	EGripLateUpdateSettings GripLateUpdateSetting,
	EGripMovementReplicationSettings GripMovementReplicationSetting,
	float GripStiffness, 
	float GripDamping,
	bool bIsSlotGrip
	)
{

	bool bIsLocalGrip = (GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive || GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep);

	if (!IsServer() && !bIsLocalGrip)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was called on the client side with a replicating grip"));
		return false;
	}

	if (!ComponentToGrip)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was passed an invalid or already gripped component"));
		return false;
	}

	if (GetIsObjectHeld(ComponentToGrip))
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController grab function was passed an already gripped component"));
		return false;
	}

	// Has to be movable to work
	if (ComponentToGrip->Mobility != EComponentMobility::Movable && (GripCollisionType != EGripCollisionType::CustomGrip && GripCollisionType != EGripCollisionType::EventsOnly))
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController tried to grip a component set to static mobility not in CustomGrip mode"));
		return false; // It is not movable, can't grip it
	}

	FBPAdvGripSettings AdvancedGripSettings;
	UObject * ObjectToCheck = NULL;
	//bool bIgnoreHandRotation = false;

	TArray<FBPGripPair> HoldingControllers;
	bool bIsHeld;
	bool bHadOriginalSettings = false;
	bool bOriginalGravity = false;
	bool bOriginalReplication = false;

	if (ComponentToGrip->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
	{
		if(IVRGripInterface::Execute_DenyGripping(ComponentToGrip))
			return false; // Interface is saying not to grip it right now

		IVRGripInterface::Execute_IsHeld(ComponentToGrip, HoldingControllers, bIsHeld);
		bool bAllowMultipleGrips = IVRGripInterface::Execute_AllowsMultipleGrips(ComponentToGrip);
		if (bIsHeld && !bAllowMultipleGrips)
		{
			return false; // Can't multiple grip this object
		}
		else if(bIsHeld)
		{
			// If we are held by multiple controllers then lets copy our original values from the first one	
			if (HoldingControllers[0].HoldingController != nullptr)
			{
				FBPActorGripInformation gripInfo;
				EBPVRResultSwitch result;
				HoldingControllers[0].HoldingController->GetGripByID(gripInfo, HoldingControllers[0].GripID, result);

				if (result != EBPVRResultSwitch::OnFailed)
				{
					bHadOriginalSettings = true;
					bOriginalGravity = gripInfo.bOriginalGravity;
					bOriginalReplication = gripInfo.bOriginalReplicatesMovement;
				}
			}
		}
		

		AdvancedGripSettings = IVRGripInterface::Execute_AdvancedGripSettings(ComponentToGrip);
		ObjectToCheck = ComponentToGrip;
	}

	//ComponentToGrip->IgnoreActorWhenMoving(this->GetOwner(), true);
	// So that events caused by sweep and the like will trigger correctly

	ComponentToGrip->AddTickPrerequisiteComponent(this);

	FBPActorGripInformation newComponentGrip;
	newComponentGrip.GripID = GetNextGripID(bIsLocalGrip);
	newComponentGrip.GripCollisionType = GripCollisionType;
	newComponentGrip.GrippedObject = ComponentToGrip;
	
	if (bHadOriginalSettings)
	{
		newComponentGrip.bOriginalReplicatesMovement = bOriginalReplication;
		newComponentGrip.bOriginalGravity = bOriginalGravity;
	}
	else
	{
		if (ComponentToGrip->GetOwner())
			newComponentGrip.bOriginalReplicatesMovement = ComponentToGrip->GetOwner()->bReplicateMovement;

		newComponentGrip.bOriginalGravity = ComponentToGrip->IsGravityEnabled();
	}
	newComponentGrip.Stiffness = GripStiffness;
	newComponentGrip.Damping = GripDamping;
	newComponentGrip.AdvancedGripSettings = AdvancedGripSettings;
	newComponentGrip.GripTargetType = EGripTargetType::ComponentGrip;
	newComponentGrip.ValueCache.bWasInitiallyRepped = true; // Set this true on authority side so we can skip a function call on tick
	newComponentGrip.bIsSlotGrip = bIsSlotGrip;
	newComponentGrip.GrippedBoneName = OptionalBoneToGripName;

	// Ignore late update setting if it doesn't make sense with the grip
	switch (newComponentGrip.GripCollisionType)
	{
	case EGripCollisionType::ManipulationGrip:
	case EGripCollisionType::ManipulationGripWithWristTwist:
	{
		newComponentGrip.GripLateUpdateSetting = EGripLateUpdateSettings::LateUpdatesAlwaysOff; // Late updates are bad for this grip
	}break;

	default:
	{
		newComponentGrip.GripLateUpdateSetting = GripLateUpdateSetting;
	}break;
	}


	if (GripMovementReplicationSetting == EGripMovementReplicationSettings::KeepOriginalMovement)
	{
		if (ComponentToGrip->GetOwner())
		{
			if (ComponentToGrip->GetOwner()->bReplicateMovement)
			{
				newComponentGrip.GripMovementReplicationSetting = EGripMovementReplicationSettings::ForceServerSideMovement;
			}
			else
			{
				newComponentGrip.GripMovementReplicationSetting = EGripMovementReplicationSettings::ForceClientSideMovement;
			}
		}
		else
			newComponentGrip.GripMovementReplicationSetting = EGripMovementReplicationSettings::ForceClientSideMovement;
	}
	else
		newComponentGrip.GripMovementReplicationSetting = GripMovementReplicationSetting;

	if (OptionalSnapToSocketName.IsValid() && ComponentToGrip->DoesSocketExist(OptionalSnapToSocketName))
	{
		// I inverse it so that laying out the sockets makes sense
		FTransform sockTrans = ComponentToGrip->GetSocketTransform(OptionalSnapToSocketName, ERelativeTransformSpace::RTS_Component);
		sockTrans.SetScale3D(FVector(1.f) / ComponentToGrip->GetComponentScale()); // Prep this so that the inverse works correctly
		newComponentGrip.RelativeTransform = sockTrans.Inverse();
		newComponentGrip.bIsSlotGrip = true; // Set this to a slot grip

		ObjectToCheck = NULL; // Null it out, socketed grips don't use this
	}
	else if (bWorldOffsetIsRelative)
	{
		if (CustomPivotComponent.IsValid() && !bIsSlotGrip)
		{
			newComponentGrip.RelativeTransform = (WorldOffset * this->GetComponentTransform()).GetRelativeTransform(CustomPivotComponent->GetComponentTransform());
		}
		else
		{
			newComponentGrip.RelativeTransform = WorldOffset;
		}
	}
	else
	{
		newComponentGrip.RelativeTransform = WorldOffset.GetRelativeTransform(GetPivotTransform());
	}

	if (!bIsLocalGrip)
	{
		int32 Index = GrippedObjects.Add(newComponentGrip);
		NotifyGrip(newComponentGrip);
	}
	else
	{
		int32 Index = LocallyGrippedObjects.Add(newComponentGrip);

		if (GetNetMode() == ENetMode::NM_Client && !IsTornOff() && newComponentGrip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive)
			Server_NotifyLocalGripAddedOrChanged(newComponentGrip);

		NotifyGrip(newComponentGrip);
	}

	return true;
}

bool UGripMotionControllerComponent::DropComponent(UPrimitiveComponent * ComponentToDrop, bool bSimulate, FVector OptionalAngularVelocity, FVector OptionalLinearVelocity)
{

	FBPActorGripInformation *GripInfo;
	
	// First check for it in the local grips	
	GripInfo = LocallyGrippedObjects.FindByKey(ComponentToDrop);

	if (GripInfo != nullptr)
	{
		return DropGrip(*GripInfo, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);
	}

	// If we aren't the server then fail out
	if (!IsServer())
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was called on the client side for a replicated grip"));
		return false;
	}

	// Now check in the server auth gripsop)
	GripInfo = GrippedObjects.FindByKey(ComponentToDrop);

	if (GripInfo != nullptr)
	{
		return DropGrip(*GripInfo, bSimulate, OptionalAngularVelocity, OptionalLinearVelocity);
	}
	else
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was passed an invalid component"));
		return false;
	}

	//return false;
}

bool UGripMotionControllerComponent::DropGrip(const FBPActorGripInformation &Grip, bool bSimulate, FVector OptionalAngularVelocity, FVector OptionalLinearVelocity)
{
	int FoundIndex = 0;
	bool bWasLocalGrip = false;
	if (!LocallyGrippedObjects.Find(Grip, FoundIndex)) // This auto checks if Actor and Component are valid in the == operator
	{
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was called on the client side for a replicated grip"));
			return false;
		}

		if (!GrippedObjects.Find(Grip, FoundIndex)) // This auto checks if Actor and Component are valid in the == operator
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was passed an invalid drop"));
			return false;
		}

		bWasLocalGrip = false;
	}
	else
		bWasLocalGrip = true;


	UPrimitiveComponent * PrimComp = nullptr;

	AActor * pActor = nullptr;
	if (bWasLocalGrip)
	{
		PrimComp = LocallyGrippedObjects[FoundIndex].GetGrippedComponent();
		pActor = LocallyGrippedObjects[FoundIndex].GetGrippedActor();
	}
	else
	{
		PrimComp = GrippedObjects[FoundIndex].GetGrippedComponent();
		pActor = GrippedObjects[FoundIndex].GetGrippedActor();
	}

	if (!PrimComp && pActor)
		PrimComp = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

	if(!PrimComp)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop function was passed an invalid drop or CleanUpBadGrip wascalled"));
		//return false;
	}
	else
	{

		// Had to move in front of deletion to properly set velocity
		if (((bWasLocalGrip && !IsLocallyControlled()) ||
			Grip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceClientSideMovement) &&
			(!OptionalLinearVelocity.IsNearlyZero() || !OptionalAngularVelocity.IsNearlyZero())
		)
		{
			PrimComp->SetPhysicsLinearVelocity(OptionalLinearVelocity);
			PrimComp->SetPhysicsAngularVelocityInDegrees(OptionalAngularVelocity);
		}
	}

	if (bWasLocalGrip)
	{
		if (GetNetMode() == ENetMode::NM_Client)
		{
			if (!IsTornOff())
			{
				FTransform_NetQuantize TransformAtDrop = FTransform::Identity;

				switch (LocallyGrippedObjects[FoundIndex].GripTargetType)
				{
				case EGripTargetType::ActorGrip:
				{
					if (AActor * GrippedActor = LocallyGrippedObjects[FoundIndex].GetGrippedActor())
					{
						TransformAtDrop = GrippedActor->GetActorTransform();
					}
				}; break;
				case EGripTargetType::ComponentGrip:
				{
					if (UPrimitiveComponent * GrippedPrim = LocallyGrippedObjects[FoundIndex].GetGrippedComponent())
					{
						TransformAtDrop = GrippedPrim->GetComponentTransform();
					}
				}break;
				default:break;
				}

				Server_NotifyLocalGripRemoved(LocallyGrippedObjects[FoundIndex].GripID, TransformAtDrop, OptionalAngularVelocity, OptionalLinearVelocity);
			}

			// Have to call this ourselves
			Drop_Implementation(LocallyGrippedObjects[FoundIndex], bSimulate);
		}
		else // Server notifyDrop it
		{
			NotifyDrop(LocallyGrippedObjects[FoundIndex], bSimulate);
		}
	}
	else
		NotifyDrop(GrippedObjects[FoundIndex], bSimulate);

	//GrippedObjects.RemoveAt(FoundIndex);		
	return true;
}

bool UGripMotionControllerComponent::DropAndSocketObject(const FTransform_NetQuantize & RelativeTransformToParent, UObject * ObjectToDrop, uint8 GripIDToDrop, USceneComponent * SocketingParent, FName OptionalSocketName, bool bWeldBodies)
{
	if (!SocketingParent)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid socketing parent"));
		return false;
	}

	if (!ObjectToDrop)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid object"));
		return false;
	}

	bool bWasLocalGrip = false;
	FBPActorGripInformation * GripInfo = nullptr;

	if (ObjectToDrop)
		GripInfo = LocallyGrippedObjects.FindByKey(ObjectToDrop);
	else if (GripIDToDrop != INVALID_VRGRIP_ID)
		GripInfo = LocallyGrippedObjects.FindByKey(GripIDToDrop);

	if(GripInfo) // This auto checks if Actor and Component are valid in the == operator
	{
		bWasLocalGrip = true;
	}
	else
	{
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was called on the client side for a replicated grip"));
			return false;
		}

		if(ObjectToDrop)
			GripInfo = GrippedObjects.FindByKey(ObjectToDrop);
		else if(GripIDToDrop != INVALID_VRGRIP_ID)
			GripInfo = GrippedObjects.FindByKey(GripIDToDrop);

		if(GripInfo) // This auto checks if Actor and Component are valid in the == operator
		{
			bWasLocalGrip = false;
		}
		else
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid drop"));
			return false;
		}
	}

	if(GripInfo)
		return DropAndSocketGrip(*GripInfo, SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies);
	
	return false;
}

bool UGripMotionControllerComponent::DropAndSocketGrip(const FBPActorGripInformation & GripToDrop, USceneComponent * SocketingParent, FName OptionalSocketName, const FTransform_NetQuantize & RelativeTransformToParent, bool bWeldBodies)
{
	if (!SocketingParent)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid socketing parent"));
		return false;
	}

	bool bWasLocalGrip = false;
	FBPActorGripInformation * GripInfo = nullptr;

	GripInfo = LocallyGrippedObjects.FindByKey(GripToDrop);
	if (GripInfo) // This auto checks if Actor and Component are valid in the == operator
	{
		bWasLocalGrip = true;
	}
	else
	{
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was called on the client side for a replicated grip"));
			return false;
		}

		GripInfo = GrippedObjects.FindByKey(GripToDrop);

		if (GripInfo) // This auto checks if Actor and Component are valid in the == operator
		{
			bWasLocalGrip = false;
		}
		else
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid drop"));
			return false;
		}
	}

	UPrimitiveComponent * PrimComp = nullptr;

	AActor * pActor = nullptr;
	if (bWasLocalGrip)
	{
		PrimComp = GripInfo->GetGrippedComponent();
		pActor = GripInfo->GetGrippedActor();
	}
	else
	{
		PrimComp = GripInfo->GetGrippedComponent();
		pActor = GripInfo->GetGrippedActor();
	}

	if (!PrimComp && pActor)
		PrimComp = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

	if (!PrimComp)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController drop and socket function was passed an invalid drop or CleanUpBadGrip wascalled"));
		//return false;
	}

	UObject * GrippedObject = GripInfo->GrippedObject;

	int PhysicsHandleIndex = INDEX_NONE;
	GetPhysicsGripIndex(*GripInfo, PhysicsHandleIndex);

	if (bWasLocalGrip)
	{
		if (GetNetMode() == ENetMode::NM_Client)
		{
			if(!IsTornOff())
				Server_NotifyDropAndSocketGrip(GripInfo->GripID, SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies);

			// Have to call this ourselves
			DropAndSocket_Implementation(*GripInfo);
			if (GrippedObject)
				Socket_Implementation(GrippedObject, (PhysicsHandleIndex != INDEX_NONE), SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies);
		}
		else // Server notifyDrop it
		{
			NotifyDropAndSocket(*GripInfo);
			if (GrippedObject)
				Socket_Implementation(GrippedObject, (PhysicsHandleIndex != INDEX_NONE), SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies);
		}
	}
	else
	{
		NotifyDropAndSocket(*GripInfo);
		if (GrippedObject)
			Socket_Implementation(GrippedObject, (PhysicsHandleIndex != INDEX_NONE), SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies);
	}

	//GrippedObjects.RemoveAt(FoundIndex);		
	return true;
}

void UGripMotionControllerComponent::SetSocketTransform(UObject* ObjectToSocket, /*USceneComponent * SocketingParent,*/ const FTransform_NetQuantize RelativeTransformToParent/*, FName OptionalSocketName, bool bWeldBodies*/)
{
	if (ObjectsWaitingForSocketUpdate.RemoveSingle(ObjectToSocket) < 1)
	{
		// I know that technically it should never happen that the pointers get reset with a uproperty
		// But does it really hurt to add this pathway anyway?
		for (int i = ObjectsWaitingForSocketUpdate.Num() - 1; i >= 0; --i)
		{
			if (ObjectsWaitingForSocketUpdate[i] == nullptr)
				ObjectsWaitingForSocketUpdate.RemoveAt(i);
		}

		return;
	}

	if (!ObjectToSocket || ObjectToSocket->IsPendingKill())
		return;

	/*FAttachmentTransformRules TransformRule = FAttachmentTransformRules::KeepWorldTransform;//KeepWorldTransform;
	TransformRule.bWeldSimulatedBodies = bWeldBodies;*/

	if (UPrimitiveComponent * root = Cast<UPrimitiveComponent>(ObjectToSocket))
	{
		//root->AttachToComponent(SocketingParent, TransformRule, OptionalSocketName);
		//root->SetRelativeTransform(RelativeTransformToParent);

		if(root->GetAttachParent())
			root->SetRelativeTransform(RelativeTransformToParent);
	}
	else if (AActor * pActor = Cast<AActor>(ObjectToSocket))
	{
		//pActor->AttachToComponent(SocketingParent, TransformRule, OptionalSocketName);
		//pActor->SetActorRelativeTransform(RelativeTransformToParent);

		if(pActor->GetAttachParentActor())
			pActor->SetActorRelativeTransform(RelativeTransformToParent);
	}
}


bool UGripMotionControllerComponent::Server_NotifyDropAndSocketGrip_Validate(uint8 GripID, USceneComponent * SocketingParent, FName OptionalSocketName, const FTransform_NetQuantize & RelativeTransformToParent, bool bWeldBodies)
{
	return true;
}

void UGripMotionControllerComponent::Server_NotifyDropAndSocketGrip_Implementation(uint8 GripID, USceneComponent * SocketingParent, FName OptionalSocketName, const FTransform_NetQuantize & RelativeTransformToParent, bool bWeldBodies)
{
	FBPActorGripInformation FoundGrip;
	EBPVRResultSwitch Result;
	
	GetGripByID(FoundGrip, GripID, Result);

	if (Result == EBPVRResultSwitch::OnFailed)
		return;

	int PhysicsHandleIndex = INDEX_NONE;
	GetPhysicsGripIndex(FoundGrip, PhysicsHandleIndex);

	if (!DropAndSocketGrip(FoundGrip, SocketingParent, OptionalSocketName, RelativeTransformToParent, bWeldBodies))
	{
		DropGrip(FoundGrip, false);
	}
	
	if (FoundGrip.GrippedObject)
		Socket_Implementation(FoundGrip.GrippedObject, (PhysicsHandleIndex != INDEX_NONE), SocketingParent, OptionalSocketName, RelativeTransformToParent);
}

void UGripMotionControllerComponent::Socket_Implementation(UObject * ObjectToSocket, bool bWasSimulating, USceneComponent * SocketingParent, FName OptionalSocketName, const FTransform_NetQuantize & RelativeTransformToParent, bool bWeldBodies)
{
	// Check for valid objects
	if (!ObjectToSocket || !SocketingParent)
		return;

	FAttachmentTransformRules TransformRule = FAttachmentTransformRules::KeepWorldTransform;//KeepWorldTransform;
	TransformRule.bWeldSimulatedBodies = bWeldBodies;

	UPrimitiveComponent * ParentPrim = Cast<UPrimitiveComponent>(SocketingParent);

	if (UPrimitiveComponent * root = Cast<UPrimitiveComponent>(ObjectToSocket))
	{
		root->AttachToComponent(SocketingParent, TransformRule, OptionalSocketName);
		root->SetRelativeTransform(RelativeTransformToParent);
	}
	else if (AActor * pActor = Cast<AActor>(ObjectToSocket))
	{
		pActor->AttachToComponent(SocketingParent, TransformRule, OptionalSocketName);
		pActor->SetActorRelativeTransform(RelativeTransformToParent);

		//if (!bRetainOwnership)
			//pActor->SetOwner(nullptr);
	}

	// It had a physics handle, I need to delay a tick and set the transform to ensure it skips a race condition
	// I may need to consider running the entire attachment in here instead in the future
	if (bWasSimulating)
	{
		ObjectsWaitingForSocketUpdate.Add(ObjectToSocket);
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UGripMotionControllerComponent::SetSocketTransform, ObjectToSocket, /*SocketingParent, */RelativeTransformToParent/*, OptionalSocketName, bWeldBodies*/));
	}
}

void UGripMotionControllerComponent::NotifyDropAndSocket_Implementation(const FBPActorGripInformation &NewDrop)
{
	// Don't do this if we are the owning player on a local grip, there is no filter for multicast to not send to owner
	if ((NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive ||
		NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep) &&
		IsLocallyControlled() &&
		GetNetMode() == ENetMode::NM_Client)
	{
		return;
	}

	DropAndSocket_Implementation(NewDrop);
}

void UGripMotionControllerComponent::DropAndSocket_Implementation(const FBPActorGripInformation &NewDrop)
{
	UGripMotionControllerComponent * HoldingController = nullptr;
	bool bIsHeld = false;

	DestroyPhysicsHandle(NewDrop);

	bool bHadGripAuthority = HasGripAuthority(NewDrop);

	UPrimitiveComponent *root = NULL;
	AActor * pActor = NULL;

	switch (NewDrop.GripTargetType)
	{
	case EGripTargetType::ActorGrip:
		//case EGripTargetType::InteractibleActorGrip:
	{
		pActor = NewDrop.GetGrippedActor();

		if (pActor)
		{
			root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

			pActor->RemoveTickPrerequisiteComponent(this);
			//this->IgnoreActorWhenMoving(pActor, false);

			if (APawn* OwningPawn = Cast<APawn>(GetOwner()))
			{
				OwningPawn->MoveIgnoreActorRemove(pActor);

				// Clearing owner out here
				// Now I am setting the owner to the owning pawn if we are one
				// This makes sure that some special replication needs are taken care of
				// Only doing this for actor grips
				// #TODO: Add the removal back in?
				//pActor->SetOwner(nullptr);
			}

			if (root)
			{
				//root->IgnoreActorWhenMoving(this->GetOwner(), false);

				// Attachment already handles both of these
				//root->UpdateComponentToWorld(); // This fixes the late update offset
				//root->SetSimulatePhysics(false);

				if ((NewDrop.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewDrop.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
					(NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
					root->SetEnableGravity(NewDrop.bOriginalGravity);
				
				// Stop Physics sim for socketing
				root->SetSimulatePhysics(false);
			}

			if (IsServer()) //&& !bSkipFullDrop)
			{
				pActor->SetReplicateMovement(NewDrop.bOriginalReplicatesMovement);
			}

			if (pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(pActor, this, NewDrop.GripID, false);

				if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
					IVRGripInterface::Execute_OnSecondaryGripRelease(pActor, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(pActor, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
								Script->OnSecondaryGripRelease(this, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

							Script->OnGripRelease(this, NewDrop, true);
						}
					}
				}

				IVRGripInterface::Execute_OnGripRelease(pActor, this, NewDrop, true);
			}
		}
	}break;

	case EGripTargetType::ComponentGrip:
		//case EGripTargetType::InteractibleComponentGrip:
	{
		root = NewDrop.GetGrippedComponent();
		if (root)
		{
			pActor = root->GetOwner();

			root->RemoveTickPrerequisiteComponent(this);
			//root->IgnoreActorWhenMoving(this->GetOwner(), false);

			// Attachment already handles both of these
			//root->UpdateComponentToWorld();
			//root->SetSimulatePhysics(false);

			if ((NewDrop.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewDrop.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
				(NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
				root->SetEnableGravity(NewDrop.bOriginalGravity);

			// Stop Physics sim for socketing
			root->SetSimulatePhysics(false);

			if (pActor)
			{
				if (IsServer() && root == pActor->GetRootComponent()) //&& !bSkipFullDrop)
				{
					pActor->SetReplicateMovement(NewDrop.bOriginalReplicatesMovement);
				}

				if (pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
				{
					IVRGripInterface::Execute_OnChildGripRelease(pActor, this, NewDrop, true);
				}
			}

			if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(root, this, NewDrop.GripID, false);

				if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
					IVRGripInterface::Execute_OnSecondaryGripRelease(root, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(root, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
								Script->OnSecondaryGripRelease(this, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

							Script->OnGripRelease(this, NewDrop, true);
						}
					}
				}

				IVRGripInterface::Execute_OnGripRelease(root, this, NewDrop, true);
			}

			// Call on child grip release on attached parent component
			if (root->GetAttachParent() && root->GetAttachParent()->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_OnChildGripRelease(root->GetAttachParent(), this, NewDrop, true);
			}
		}
	}break;
	}

	// Copy over the information instead of working with a reference for the OnDroppedBroadcast
	FBPActorGripInformation DropBroadcastData = NewDrop;

	int fIndex = 0;
	if (LocallyGrippedObjects.Find(NewDrop, fIndex))
	{
		if (HasGripAuthority(NewDrop) || GetNetMode() < ENetMode::NM_Client)
		{
			LocallyGrippedObjects.RemoveAt(fIndex);
		}
		else
			LocallyGrippedObjects[fIndex].bIsPaused = true; // Pause it instead of dropping, dropping can corrupt the array in rare cases
	}
	else
	{
		fIndex = 0;
		if (GrippedObjects.Find(NewDrop, fIndex))
		{
			if (HasGripAuthority(NewDrop) || GetNetMode() < ENetMode::NM_Client)
			{
				GrippedObjects.RemoveAt(fIndex);
			}
			else
				GrippedObjects[fIndex].bIsPaused = true; // Pause it instead of dropping, dropping can corrupt the array in rare cases
		}
	}

	// Broadcast a new drop
	OnDroppedObject.Broadcast(DropBroadcastData);
}


// No longer an RPC, now is called from RepNotify so that joining clients also correctly set up grips
bool UGripMotionControllerComponent::NotifyGrip(FBPActorGripInformation &NewGrip, bool bIsReInit)
{
	UPrimitiveComponent *root = NULL;
	AActor *pActor = NULL;

	switch (NewGrip.GripTargetType)
	{
	case EGripTargetType::ActorGrip:
	//case EGripTargetType::InteractibleActorGrip:
	{
		pActor = NewGrip.GetGrippedActor();

		if (pActor)
		{
			root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

			if (APawn* OwningPawn = Cast<APawn>(GetOwner()))
			{
				OwningPawn->MoveIgnoreActorAdd(pActor);

				// Now I am setting the owner to the owning pawn if we are one
				// This makes sure that some special replication needs are taken care of
				// Only doing this for actor grips
				if(NewGrip.AdvancedGripSettings.bSetOwnerOnGrip)
					pActor->SetOwner(OwningPawn);
			}

			if (!bIsReInit && pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(pActor, this, NewGrip.GripID, true);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(pActor, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							Script->OnGrip(this, NewGrip);
						}
					}
				}

				IVRGripInterface::Execute_OnGrip(pActor, this, NewGrip);
			}

			if (root)
			{
				if (NewGrip.GripCollisionType != EGripCollisionType::EventsOnly)
				{
					// Have to turn off gravity locally
					if ((NewGrip.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewGrip.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
						(NewGrip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
						root->SetEnableGravity(false);
				}
				//root->IgnoreActorWhenMoving(this->GetOwner(), true);
			}


		}
		else
			return false;
	}break;

	case EGripTargetType::ComponentGrip:
	//case EGripTargetType::InteractibleComponentGrip:
	{
		root = NewGrip.GetGrippedComponent();

		if (root)
		{
			pActor = root->GetOwner();

			if (!bIsReInit && root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(root, this, NewGrip.GripID, true);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(root, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							Script->OnGrip(this, NewGrip);
						}
					}
				}
				
				IVRGripInterface::Execute_OnGrip(root, this, NewGrip);
			}

			if (pActor)
			{
				/*if (APawn* OwningPawn = Cast<APawn>(GetOwner()))
				{
					OwningPawn->MoveIgnoreActorAdd(root->GetOwner());
				}*/

				if (!bIsReInit && pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
				{
					IVRGripInterface::Execute_OnChildGrip(pActor, this, NewGrip);
				}

			}

			// Call OnChildGrip for attached grip parent
			if (!bIsReInit && root->GetAttachParent() && root->GetAttachParent()->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_OnChildGrip(root->GetAttachParent(), this, NewGrip);
			}

			if (NewGrip.GripCollisionType != EGripCollisionType::EventsOnly)
			{
				if ((NewGrip.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewGrip.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
					(NewGrip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
					root->SetEnableGravity(false);
			}

			//root->IgnoreActorWhenMoving(this->GetOwner(), true);
		}
		else
			return false;
	}break;
	}

	switch (NewGrip.GripMovementReplicationSetting)
	{
	case EGripMovementReplicationSettings::ForceClientSideMovement:
	case EGripMovementReplicationSettings::ClientSide_Authoritive:
	case EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep:
	{
		if (NewGrip.GripCollisionType != EGripCollisionType::EventsOnly)
		{
			if (IsServer() && pActor && ((NewGrip.GripTargetType == EGripTargetType::ActorGrip) || (root && root == pActor->GetRootComponent())))
			{
				pActor->SetReplicateMovement(false);
			}
			if (root)
			{

				// #TODO: This is a hack until Epic fixes their new physics replication code
				//		  It forces the replication target to null on grip if we aren't repping movement.
#if WITH_PHYSX
				if (UWorld* World = GetWorld())
				{
					if (FPhysScene* PhysScene = World->GetPhysicsScene())
					{
						if (FPhysicsReplication* PhysicsReplication = PhysScene->GetPhysicsReplication())
						{
							FBodyInstance* BI = root->GetBodyInstance(NewGrip.GrippedBoneName);
							if (BI && BI->IsInstanceSimulatingPhysics())
							{
								PhysicsReplication->RemoveReplicatedTarget(root);
								//PhysicsReplication->SetReplicatedTarget(this, BoneName, UpdatedState);
							}
						}
					}
				}
#endif
			}
		}

	}break; 

	case EGripMovementReplicationSettings::ForceServerSideMovement:
	{
		if (NewGrip.GripCollisionType != EGripCollisionType::EventsOnly)
		{
			if (IsServer() && pActor && ((NewGrip.GripTargetType == EGripTargetType::ActorGrip) || (root && root == pActor->GetRootComponent())))
			{
				pActor->SetReplicateMovement(true);
			}
		}
	}break;

	case EGripMovementReplicationSettings::KeepOriginalMovement:
	default:
	{}break;
	}

	bool bHasMovementAuthority = HasGripMovementAuthority(NewGrip);

	switch (NewGrip.GripCollisionType)
	{
	case EGripCollisionType::InteractiveCollisionWithPhysics:
	case EGripCollisionType::InteractiveHybridCollisionWithPhysics:
	case EGripCollisionType::ManipulationGrip:
	case EGripCollisionType::ManipulationGripWithWristTwist:
	{
		if (bHasMovementAuthority)
		{
			SetUpPhysicsHandle(NewGrip);
		}
	} break;

	// Skip collision intersects with these types, they dont need it
	case EGripCollisionType::EventsOnly:
	case EGripCollisionType::CustomGrip:
	{		
		// Should have never been turning off physics here, simulating is a valid custom grip state
		//if (root)
			//root->SetSimulatePhysics(false);

	} break;

	case EGripCollisionType::AttachmentGrip:
	{
		if (root)
			root->SetSimulatePhysics(false);

		// Move it to the correct location automatically
		if (bHasMovementAuthority)
			TeleportMoveGrip(NewGrip);

		if(bHasMovementAuthority || IsServer())
			root->AttachToComponent(CustomPivotComponent.IsValid() ? CustomPivotComponent.Get() : this, FAttachmentTransformRules::KeepWorldTransform);

	}break;

	case EGripCollisionType::PhysicsOnly:
	case EGripCollisionType::SweepWithPhysics:
	case EGripCollisionType::InteractiveHybridCollisionWithSweep:
	case EGripCollisionType::InteractiveCollisionWithSweep:
	default: 
	{
		if (root)
			root->SetSimulatePhysics(false);

		// Move it to the correct location automatically
		if (bHasMovementAuthority)
			TeleportMoveGrip(NewGrip);
	} break;

	}

	if (!bIsReInit)
	{
		// Broadcast a new grip
		OnGrippedObject.Broadcast(NewGrip);
	}

	return true;
}

void UGripMotionControllerComponent::NotifyDrop_Implementation(const FBPActorGripInformation &NewDrop, bool bSimulate)
{
	// Don't do this if we are the owning player on a local grip, there is no filter for multicast to not send to owner
	if ((NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive || 
		NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep) && 
		IsLocallyControlled() && 
		GetNetMode() == ENetMode::NM_Client)
	{
		return;
	}

	Drop_Implementation(NewDrop, bSimulate);
}

void UGripMotionControllerComponent::Drop_Implementation(const FBPActorGripInformation &NewDrop, bool bSimulate)
{

	bool bSkipFullDrop = false;
	bool bHadAnotherSelfGrip = false;
	TArray<FBPGripPair> HoldingControllers;
	bool bIsHeld = false;

	// Check if a different controller is holding it
	if(NewDrop.GrippedObject && NewDrop.GrippedObject->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		IVRGripInterface::Execute_IsHeld(NewDrop.GrippedObject, HoldingControllers, bIsHeld);

	if (bIsHeld && (!HoldingControllers.Contains(this) || HoldingControllers.Num() > 1))
	{
		// Skip the full drop if held
		bSkipFullDrop = true;
	}	
	else // Now check for this same hand with duplicate grips on this object
	{
		for (int i = 0; i < LocallyGrippedObjects.Num(); ++i)
		{
			if (LocallyGrippedObjects[i].GrippedObject == NewDrop.GrippedObject && LocallyGrippedObjects[i].GripID != NewDrop.GripID)
			{
				bSkipFullDrop = true;
				bHadAnotherSelfGrip = true;
			}
		}
		for (int i = 0; i < GrippedObjects.Num(); ++i)
		{
			if (GrippedObjects[i].GrippedObject == NewDrop.GrippedObject && GrippedObjects[i].GripID != NewDrop.GripID)
			{
				bSkipFullDrop = true;
				bHadAnotherSelfGrip = true;
			}
		}
	}

	DestroyPhysicsHandle(NewDrop, bHadAnotherSelfGrip);

	bool bHadGripAuthority = HasGripAuthority(NewDrop);

	UPrimitiveComponent *root = NULL;
	AActor * pActor = NULL;

	switch (NewDrop.GripTargetType)
	{
	case EGripTargetType::ActorGrip:
		//case EGripTargetType::InteractibleActorGrip:
	{
		pActor = NewDrop.GetGrippedActor();

		if (pActor)
		{
			root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

			if (!bSkipFullDrop)
			{
				pActor->RemoveTickPrerequisiteComponent(this);
				//this->IgnoreActorWhenMoving(pActor, false);

				if (APawn* OwningPawn = Cast<APawn>(GetOwner()))
				{
					OwningPawn->MoveIgnoreActorRemove(pActor);

					// Clearing owner out here
					// Now I am setting the owner to the owning pawn if we are one
					// This makes sure that some special replication needs are taken care of
					// Only doing this for actor grips
					// #TODO: Add the removal back in?
					//pActor->SetOwner(nullptr);
				}

				if (root)
				{

					if (NewDrop.GripCollisionType == EGripCollisionType::AttachmentGrip && (HasGripAuthority(NewDrop) || IsServer()))
						root->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

					//root->IgnoreActorWhenMoving(this->GetOwner(), false);

					if (NewDrop.GripCollisionType != EGripCollisionType::EventsOnly)
					{
						if (IsServer() || bHadGripAuthority || !NewDrop.bOriginalReplicatesMovement || !pActor->GetIsReplicated())
						{
							if (root->IsSimulatingPhysics() != bSimulate)
							{
								root->SetSimulatePhysics(bSimulate);
							}

							if (bSimulate)
								root->WakeAllRigidBodies();
						}

						root->UpdateComponentToWorld(); // This fixes the late update offset
					}

					/*if (NewDrop.GrippedBoneName == NAME_None)
					{
						root->SetSimulatePhysics(bSimulate);
						root->UpdateComponentToWorld(); // This fixes the late update offset
						if (bSimulate)
							root->WakeAllRigidBodies();
					}
					else
					{
						USkeletalMeshComponent * skele = Cast<USkeletalMeshComponent>(root);
						if (skele)
						{
							skele->SetAllBodiesBelowSimulatePhysics(NewDrop.GrippedBoneName, bSimulate);
							root->UpdateComponentToWorld(); // This fixes the late update offset
						}
						else
						{
							root->SetSimulatePhysics(bSimulate);
							root->UpdateComponentToWorld(); // This fixes the late update offset
							if (bSimulate)
								root->WakeAllRigidBodies();
						}
					}*/

					if (NewDrop.GripCollisionType != EGripCollisionType::EventsOnly)
					{
						if ((NewDrop.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewDrop.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
							(NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
							root->SetEnableGravity(NewDrop.bOriginalGravity);
					}
				}
			}

			if (IsServer() && !bSkipFullDrop)
			{
				pActor->SetReplicateMovement(NewDrop.bOriginalReplicatesMovement);
			}

			if (pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(pActor, this, NewDrop.GripID, false);

				if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
					IVRGripInterface::Execute_OnSecondaryGripRelease(pActor, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(pActor, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
								Script->OnSecondaryGripRelease(this, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

							Script->OnGripRelease(this, NewDrop, false);
						}
					}
				}

				IVRGripInterface::Execute_OnGripRelease(pActor, this, NewDrop, false);
			}
		}
	}break;

	case EGripTargetType::ComponentGrip:
		//case EGripTargetType::InteractibleComponentGrip:
	{
		root = NewDrop.GetGrippedComponent();
		if (root)
		{
			pActor = root->GetOwner();

			if (!bSkipFullDrop)
			{
				root->RemoveTickPrerequisiteComponent(this);

				/*if (APawn* OwningPawn = Cast<APawn>(GetOwner()))
				{
					OwningPawn->MoveIgnoreActorRemove(pActor);
				}*/

				if (NewDrop.GripCollisionType == EGripCollisionType::AttachmentGrip && (HasGripAuthority(NewDrop) || IsServer()))
					root->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

				//root->IgnoreActorWhenMoving(this->GetOwner(), false);

				if (NewDrop.GripCollisionType != EGripCollisionType::EventsOnly)
				{
					// Need to set simulation in all of these cases, including if it isn't the root component (simulation isn't replicated on non roots)
					if (IsServer() || bHadGripAuthority || !NewDrop.bOriginalReplicatesMovement || (pActor && (pActor->GetRootComponent() != root || !pActor->GetIsReplicated())))
					{
						if (root->IsSimulatingPhysics() != bSimulate)
						{
							root->SetSimulatePhysics(bSimulate);
						}

						if (bSimulate)
							root->WakeAllRigidBodies();
					}

					root->UpdateComponentToWorld(); // This fixes the late update offset
				}
				/*if (NewDrop.GrippedBoneName == NAME_None)
				{
					root->SetSimulatePhysics(bSimulate);
					root->UpdateComponentToWorld(); // This fixes the late update offset
					if (bSimulate)
						root->WakeAllRigidBodies();
				}
				else
				{
					USkeletalMeshComponent * skele = Cast<USkeletalMeshComponent>(root);
					if (skele)
					{
						skele->SetAllBodiesBelowSimulatePhysics(NewDrop.GrippedBoneName, bSimulate);
						root->UpdateComponentToWorld(); // This fixes the late update offset
					}
					else
					{
						root->SetSimulatePhysics(bSimulate);
						root->UpdateComponentToWorld(); // This fixes the late update offset
						if (bSimulate)
							root->WakeAllRigidBodies();
					}
				}*/

				if (NewDrop.GripCollisionType != EGripCollisionType::EventsOnly)
				{
					if ((NewDrop.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewDrop.AdvancedGripSettings.PhysicsSettings.bTurnOffGravityDuringGrip) ||
						(NewDrop.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !IsServer()))
						root->SetEnableGravity(NewDrop.bOriginalGravity);
				}
			}

			if (pActor)
			{
				if (IsServer() && root == pActor->GetRootComponent() && !bSkipFullDrop)
				{
					pActor->SetReplicateMovement(NewDrop.bOriginalReplicatesMovement);
				}

				if (pActor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
				{
					IVRGripInterface::Execute_OnChildGripRelease(pActor, this, NewDrop, false);
				}

			}

			if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_SetHeld(root, this, NewDrop.GripID, false);

				if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
					IVRGripInterface::Execute_OnSecondaryGripRelease(root, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

				TArray<UVRGripScriptBase*> GripScripts;
				if (IVRGripInterface::Execute_GetGripScripts(root, GripScripts))
				{
					for (UVRGripScriptBase* Script : GripScripts)
					{
						if (Script)
						{
							if (NewDrop.SecondaryGripInfo.bHasSecondaryAttachment)
								Script->OnSecondaryGripRelease(this, NewDrop.SecondaryGripInfo.SecondaryAttachment, NewDrop);

							Script->OnGripRelease(this, NewDrop, false);
						}
					}
				}

				IVRGripInterface::Execute_OnGripRelease(root, this, NewDrop, false);
			}

			// Call on child grip release on attached parent component
			if (root->GetAttachParent() && root->GetAttachParent()->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
			{
				IVRGripInterface::Execute_OnChildGripRelease(root->GetAttachParent(), this, NewDrop, false);
			}
		}
	}break;
	}

	// Copy over the information instead of working with a reference for the OnDroppedBroadcast
	FBPActorGripInformation DropBroadcastData = NewDrop;

	int fIndex = 0;
	if (LocallyGrippedObjects.Find(NewDrop, fIndex))
	{
		if (HasGripAuthority(NewDrop) || GetNetMode() < ENetMode::NM_Client)
		{
			LocallyGrippedObjects.RemoveAt(fIndex);
		}
		else
			LocallyGrippedObjects[fIndex].bIsPaused = true; // Pause it instead of dropping, dropping can corrupt the array in rare cases
	}
	else
	{
		fIndex = 0;
		if (GrippedObjects.Find(NewDrop, fIndex))
		{
			if (HasGripAuthority(NewDrop) || GetNetMode() < ENetMode::NM_Client)
			{
				GrippedObjects.RemoveAt(fIndex);
			}
			else
				GrippedObjects[fIndex].bIsPaused = true; // Pause it instead of dropping, dropping can corrupt the array in rare cases
		}
	}

	// Broadcast a new drop
	OnDroppedObject.Broadcast(DropBroadcastData);
}

bool UGripMotionControllerComponent::BP_IsLocallyControlled()
{
	return IsLocallyControlled();
}

bool UGripMotionControllerComponent::BP_HasGripAuthority(const FBPActorGripInformation &Grip)
{
	return HasGripAuthority(Grip);
}

bool UGripMotionControllerComponent::BP_HasGripMovementAuthority(const FBPActorGripInformation &Grip)
{
	return HasGripMovementAuthority(Grip);
}

bool UGripMotionControllerComponent::AddSecondaryAttachmentPoint(UObject * GrippedObjectToAddAttachment, USceneComponent * SecondaryPointComponent, const FTransform & OriginalTransform, bool bTransformIsAlreadyRelative, float LerpToTime,/* float SecondarySmoothingScaler,*/ bool bIsSlotGrip)
{
	if (!GrippedObjectToAddAttachment || !SecondaryPointComponent || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripToUse = nullptr;

	GripToUse = LocallyGrippedObjects.FindByKey(GrippedObjectToAddAttachment);

	// Search replicated grips if not found in local
	if (!GripToUse)
	{
		// Replicated grips need to be called from server side
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController add secondary attachment function was called on the client side with a replicated grip"));
			return false;
		}

		GripToUse = GrippedObjects.FindByKey(GrippedObjectToAddAttachment);
	}

	if (GripToUse)
	{
		return AddSecondaryAttachmentToGrip(*GripToUse, SecondaryPointComponent, OriginalTransform, bTransformIsAlreadyRelative, LerpToTime, bIsSlotGrip);
	}

	return false;
}

bool UGripMotionControllerComponent::AddSecondaryAttachmentToGrip(const FBPActorGripInformation & GripToAddAttachment, USceneComponent * SecondaryPointComponent, const FTransform &OriginalTransform, bool bTransformIsAlreadyRelative, float LerpToTime, bool bIsSlotGrip)
{
	if (!GripToAddAttachment.GrippedObject || GripToAddAttachment.GripID == INVALID_VRGRIP_ID || !SecondaryPointComponent || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripToUse = nullptr;

	GripToUse = LocallyGrippedObjects.FindByKey(GripToAddAttachment.GripID);

	// Search replicated grips if not found in local
	if (!GripToUse)
	{
		// Replicated grips need to be called from server side
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController add secondary attachment function was called on the client side with a replicated grip"));
			return false;
		}

		GripToUse = GrippedObjects.FindByKey(GripToAddAttachment.GripID);
	}

	if (!GripToUse || !GripToUse->GrippedObject)
		return false;

	bool bGrippedObjectIsInterfaced = GripToUse->GrippedObject->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass());

	if (bGrippedObjectIsInterfaced)
	{
		ESecondaryGripType SecondaryType = IVRGripInterface::Execute_SecondaryGripType(GripToUse->GrippedObject);

		if (SecondaryType == ESecondaryGripType::SG_None)
			return false;
	}

	UPrimitiveComponent * root = nullptr;

	switch (GripToUse->GripTargetType)
	{
	case EGripTargetType::ActorGrip:
	{
		AActor * pActor = GripToUse->GetGrippedActor();

		if (pActor)
		{
			root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());
		}
	}
	break;
	case EGripTargetType::ComponentGrip:
	{
		root = GripToUse->GetGrippedComponent();
	}
	break;
	}

	if (!root)
	{
		UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController add secondary attachment function was unable to get root component or gripped component."));
		return false;
	}

	if (bTransformIsAlreadyRelative)
		GripToUse->SecondaryGripInfo.SecondaryRelativeTransform = OriginalTransform;
	else
		GripToUse->SecondaryGripInfo.SecondaryRelativeTransform = OriginalTransform.GetRelativeTransform(root->GetComponentTransform());

	GripToUse->SecondaryGripInfo.SecondaryAttachment = SecondaryPointComponent;
	GripToUse->SecondaryGripInfo.bHasSecondaryAttachment = true;
	GripToUse->SecondaryGripInfo.SecondaryGripDistance = 0.0f;

	/*const UVRGlobalSettings& VRSettings = *GetDefault<UVRGlobalSettings>();
	GripToUse->AdvancedGripSettings.SecondaryGripSettings.SecondarySmoothing.CutoffSlope = VRSettings.OneEuroCutoffSlope;
	GripToUse->AdvancedGripSettings.SecondaryGripSettings.SecondarySmoothing.DeltaCutoff = VRSettings.OneEuroDeltaCutoff;
	GripToUse->AdvancedGripSettings.SecondaryGripSettings.SecondarySmoothing.MinCutoff = VRSettings.OneEuroMinCutoff;

	GripToUse->AdvancedGripSettings.SecondaryGripSettings.SecondarySmoothing.ResetSmoothingFilter();*/
	//	GripToUse->SecondaryGripInfo.SecondarySmoothingScaler = FMath::Clamp(SecondarySmoothingScaler, 0.01f, 1.0f);
	GripToUse->SecondaryGripInfo.bIsSlotGrip = bIsSlotGrip;

	if (GripToUse->SecondaryGripInfo.GripLerpState == EGripLerpState::EndLerp)
		LerpToTime = 0.0f;

	if (LerpToTime > 0.0f)
	{
		GripToUse->SecondaryGripInfo.LerpToRate = LerpToTime;
		GripToUse->SecondaryGripInfo.GripLerpState = EGripLerpState::StartLerp;
		GripToUse->SecondaryGripInfo.curLerp = LerpToTime;
	}

	if (bGrippedObjectIsInterfaced)
	{
		IVRGripInterface::Execute_OnSecondaryGrip(GripToUse->GrippedObject, SecondaryPointComponent, *GripToUse);

		TArray<UVRGripScriptBase*> GripScripts;
		if (IVRGripInterface::Execute_GetGripScripts(GripToUse->GrippedObject, GripScripts))
		{
			for (UVRGripScriptBase* Script : GripScripts)
			{
				if (Script)
				{
					Script->OnSecondaryGrip(this, SecondaryPointComponent, *GripToUse);
				}
			}
		}
	}

	if (GripToUse->GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive && GetNetMode() == ENetMode::NM_Client && !IsTornOff())
	{
		Server_NotifySecondaryAttachmentChanged(GripToUse->GripID, GripToUse->SecondaryGripInfo);
	}

	GripToUse = nullptr;

	return true;
}

bool UGripMotionControllerComponent::RemoveSecondaryAttachmentPoint(UObject * GrippedObjectToRemoveAttachment, float LerpToTime)
{
	if (!GrippedObjectToRemoveAttachment || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripToUse = nullptr;

	// Duplicating the logic for each array for now
	GripToUse = LocallyGrippedObjects.FindByKey(GrippedObjectToRemoveAttachment);

	// Check replicated grips if it wasn't found in local
	if (!GripToUse)
	{
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController remove secondary attachment function was called on the client side for a replicating grip"));
			return false;
		}

		GripToUse = GrippedObjects.FindByKey(GrippedObjectToRemoveAttachment);
	}

	// Handle the grip if it was found
	if (GripToUse && GripToUse->GrippedObject)
	{
		return RemoveSecondaryAttachmentFromGrip(*GripToUse, LerpToTime);
	}

	return false;
}

bool UGripMotionControllerComponent::RemoveSecondaryAttachmentFromGrip(const FBPActorGripInformation & GripToRemoveAttachment, float LerpToTime)
{
	if (!GripToRemoveAttachment.GrippedObject || GripToRemoveAttachment.GripID == INVALID_VRGRIP_ID || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripToUse = nullptr;

	// Duplicating the logic for each array for now
	GripToUse = LocallyGrippedObjects.FindByKey(GripToRemoveAttachment.GripID);

	// Check replicated grips if it wasn't found in local
	if (!GripToUse)
	{
		if (!IsServer())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("VRGripMotionController remove secondary attachment function was called on the client side for a replicating grip"));
			return false;
		}

		GripToUse = GrippedObjects.FindByKey(GripToRemoveAttachment.GripID);
	}

	// Handle the grip if it was found
	if (GripToUse && GripToUse->GrippedObject)
	{
		if (GripToUse->SecondaryGripInfo.GripLerpState == EGripLerpState::StartLerp)
			LerpToTime = 0.0f;

		//if (LerpToTime > 0.0f)
		//{
		UPrimitiveComponent * primComp = nullptr;

		switch (GripToUse->GripTargetType)
		{
		case EGripTargetType::ComponentGrip:
		{
			primComp = GripToUse->GetGrippedComponent();
		}break;
		case EGripTargetType::ActorGrip:
		{
			AActor * pActor = GripToUse->GetGrippedActor();
			if (pActor)
				primComp = Cast<UPrimitiveComponent>(pActor->GetRootComponent());
		} break;
		}

		bool bGripObjectHasInterface = GripToUse->GrippedObject->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass());

		ESecondaryGripType SecondaryType = ESecondaryGripType::SG_None;
		if (bGripObjectHasInterface)
		{
			SecondaryType = IVRGripInterface::Execute_SecondaryGripType(GripToUse->GrippedObject);
			//else if (SecondaryType == ESecondaryGripType::SG_FreeWithScaling || SecondaryType == ESecondaryGripType::SG_SlotOnlyWithScaling)
			//LerpToTime = 0.0f;
		}

		if (primComp)
		{
			switch (SecondaryType)
			{
				// All of these retain the position on release
			case ESecondaryGripType::SG_FreeWithScaling_Retain:
			case ESecondaryGripType::SG_SlotOnlyWithScaling_Retain:
			case ESecondaryGripType::SG_Free_Retain:
			case ESecondaryGripType::SG_SlotOnly_Retain:
			case ESecondaryGripType::SG_ScalingOnly:
			{
				GripToUse->RelativeTransform = primComp->GetComponentTransform().GetRelativeTransform(GetPivotTransform());
				GripToUse->SecondaryGripInfo.LerpToRate = 0.0f;
				GripToUse->SecondaryGripInfo.GripLerpState = EGripLerpState::NotLerping;
			}break;
			default:
			{
				if (LerpToTime > 0.0f)
				{
					// #TODO: This had a hitch in it just prior to lerping back, fix it eventually and allow lerping from scaling secondaries
					//GripToUse->RelativeTransform.SetScale3D(GripToUse->RelativeTransform.GetScale3D() * FVector(GripToUse->SecondaryScaler));
					GripToUse->SecondaryGripInfo.LerpToRate = LerpToTime;
					GripToUse->SecondaryGripInfo.GripLerpState = EGripLerpState::EndLerp;
					GripToUse->SecondaryGripInfo.curLerp = LerpToTime;
				}
			}break;
			}

		}
		else
		{
			GripToUse->SecondaryGripInfo.LerpToRate = 0.0f;
			GripToUse->SecondaryGripInfo.GripLerpState = EGripLerpState::NotLerping;
		}

		if (bGripObjectHasInterface)
		{
			IVRGripInterface::Execute_OnSecondaryGripRelease(GripToUse->GrippedObject, GripToUse->SecondaryGripInfo.SecondaryAttachment, *GripToUse);

			TArray<UVRGripScriptBase*> GripScripts;
			if (IVRGripInterface::Execute_GetGripScripts(GripToUse->GrippedObject, GripScripts))
			{
				for (UVRGripScriptBase* Script : GripScripts)
				{
					if (Script)
					{
						Script->OnSecondaryGripRelease(this, GripToUse->SecondaryGripInfo.SecondaryAttachment, *GripToUse);
					}
				}
			}
		}

		GripToUse->SecondaryGripInfo.SecondaryAttachment = nullptr;
		GripToUse->SecondaryGripInfo.bHasSecondaryAttachment = false;

		if (GripToUse->GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive && GetNetMode() == ENetMode::NM_Client)
		{
			switch (SecondaryType)
			{
				// All of these retain the position on release
			case ESecondaryGripType::SG_FreeWithScaling_Retain:
			case ESecondaryGripType::SG_SlotOnlyWithScaling_Retain:
			case ESecondaryGripType::SG_Free_Retain:
			case ESecondaryGripType::SG_SlotOnly_Retain:
			case ESecondaryGripType::SG_ScalingOnly:
			{
				if (!IsTornOff())
					Server_NotifySecondaryAttachmentChanged_Retain(GripToUse->GripID, GripToUse->SecondaryGripInfo, GripToUse->RelativeTransform);
			}break;
			default:
			{
				if (!IsTornOff())
					Server_NotifySecondaryAttachmentChanged(GripToUse->GripID, GripToUse->SecondaryGripInfo);
			}break;
			}

		}

		GripToUse = nullptr;
		return true;
	}

	return false;
}

bool UGripMotionControllerComponent::TeleportMoveGrippedActor(AActor * GrippedActorToMove, bool bTeleportPhysicsGrips)
{
	if (!GrippedActorToMove || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripInfo = LocallyGrippedObjects.FindByKey(GrippedActorToMove);
	if (!GripInfo)
		GrippedObjects.FindByKey(GrippedActorToMove);

	if (GripInfo)
	{
		return TeleportMoveGrip(*GripInfo, bTeleportPhysicsGrips);
	}

	return false;
}

bool UGripMotionControllerComponent::TeleportMoveGrippedComponent(UPrimitiveComponent * ComponentToMove, bool bTeleportPhysicsGrips)
{
	if (!ComponentToMove || (!GrippedObjects.Num() && !LocallyGrippedObjects.Num()))
		return false;

	FBPActorGripInformation * GripInfo = LocallyGrippedObjects.FindByKey(ComponentToMove);
	if (!GripInfo)
		GrippedObjects.FindByKey(ComponentToMove);

	if (GripInfo)
	{
		return TeleportMoveGrip(*GripInfo, bTeleportPhysicsGrips);
	}

	return false;
}

bool UGripMotionControllerComponent::TeleportMoveGrip(FBPActorGripInformation &Grip, bool bTeleportPhysicsGrips, bool bIsForPostTeleport)
{
	FTransform EmptyTransform = FTransform::Identity;
	return TeleportMoveGrip_Impl(Grip, bTeleportPhysicsGrips, bIsForPostTeleport, EmptyTransform);
}

bool UGripMotionControllerComponent::TeleportMoveGrip_Impl(FBPActorGripInformation &Grip, bool bTeleportPhysicsGrips, bool bIsForPostTeleport, FTransform & OptionalTransform)
{
	bool bHasMovementAuthority = HasGripMovementAuthority(Grip);

	if (!bHasMovementAuthority)
		return false;
		

	UPrimitiveComponent * PrimComp = NULL;
	AActor * actor = NULL;

	switch (Grip.GripTargetType)
	{
	case EGripTargetType::ActorGrip:
	//case EGripTargetType::InteractibleActorGrip:
	{
		actor = Grip.GetGrippedActor();
		if (actor)
		{
			PrimComp = Cast<UPrimitiveComponent>(actor->GetRootComponent());
		}
	}break;

	case EGripTargetType::ComponentGrip:
	//case EGripTargetType::InteractibleComponentGrip:
	{
		PrimComp = Grip.GetGrippedComponent();

		if(PrimComp)
		actor = PrimComp->GetOwner();
	}break;

	}

	if (!PrimComp || !actor)
		return false;

	// Check if either implements the interface
	bool bRootHasInterface = false;
	bool bActorHasInterface = false;

	if (PrimComp->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
	{
		bRootHasInterface = true;
	}
	if (actor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
	{
		// Actor grip interface is checked after component
		bActorHasInterface = true;
	}


	// Only use with actual teleporting

	EGripInterfaceTeleportBehavior TeleportBehavior = EGripInterfaceTeleportBehavior::TeleportAllComponents;
	bool bSimulateOnDrop = false;

	// Check for interaction interface
	if (bRootHasInterface)
	{
		TeleportBehavior = IVRGripInterface::Execute_TeleportBehavior(PrimComp);
		bSimulateOnDrop = IVRGripInterface::Execute_SimulateOnDrop(PrimComp);
	}
	else if (bActorHasInterface)
	{
		// Actor grip interface is checked after component
		TeleportBehavior = IVRGripInterface::Execute_TeleportBehavior(actor);
		bSimulateOnDrop = IVRGripInterface::Execute_SimulateOnDrop(actor);
	}

	if (bIsForPostTeleport)
	{
		if (TeleportBehavior == EGripInterfaceTeleportBehavior::OnlyTeleportRootComponent)
		{
			if (AActor * owner = PrimComp->GetOwner())
			{
				if (PrimComp != owner->GetRootComponent())
				{
					return false;
				}
			}
		}
		else if (TeleportBehavior == EGripInterfaceTeleportBehavior::DropOnTeleport)
		{
			if (IsServer() ||
				Grip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive ||
				Grip.GripMovementReplicationSetting == EGripMovementReplicationSettings::ClientSide_Authoritive_NoRep)
			{
				DropObjectByInterface(Grip.GrippedObject);
			}
			
			return false; // Didn't teleport
		}
		else if (TeleportBehavior == EGripInterfaceTeleportBehavior::DontTeleport)
		{
			return false; // Didn't teleport
		}
	}
	else
	{
		switch (TeleportBehavior)
		{
		case EGripInterfaceTeleportBehavior::DontTeleport:
		case EGripInterfaceTeleportBehavior::DropOnTeleport:
		{
			return false;
		}break;
		default:break;
		}
	}

	FTransform WorldTransform;
	FTransform ParentTransform = GetPivotTransform();

	FBPActorGripInformation copyGrip = Grip;
	
	if (!OptionalTransform.Equals(FTransform::Identity))
		WorldTransform = OptionalTransform;
	else
	{
		TArray<UVRGripScriptBase*> Scripts;

		if (bRootHasInterface)
		{
			IVRGripInterface::Execute_GetGripScripts(PrimComp, Scripts);
		}
		else if (bActorHasInterface)
		{
			IVRGripInterface::Execute_GetGripScripts(actor, Scripts);
		}

		bool bForceADrop = false;
		bool bHadValidWorldTransform = GetGripWorldTransform(Scripts, 0.0f, WorldTransform, ParentTransform, copyGrip, actor, PrimComp, bRootHasInterface, bActorHasInterface, true, bForceADrop);
	
		if (!bHadValidWorldTransform)
			return false;
	}

	// Saving this out prior as we are still setting our physics thread to the correct value, the delta is only applied to the object
	FTransform physicsTrans = WorldTransform;
	if (TeleportBehavior == EGripInterfaceTeleportBehavior::DeltaTeleportation && !Grip.LastWorldTransform.Equals(FTransform::Identity))
	{
		FVector DeltaVec = WorldTransform.GetTranslation() - Grip.LastWorldTransform.GetTranslation();
		FQuat DeltaQuat = Grip.LastWorldTransform.GetRotation().Inverse() * WorldTransform.GetRotation();

		WorldTransform = PrimComp->GetComponentTransform();
		WorldTransform.AddToTranslation(DeltaVec);
		WorldTransform.ConcatenateRotation(DeltaQuat);
	}

	// Need to use WITH teleport for this function so that the velocity isn't updated and without sweep so that they don't collide
	
	FBPActorPhysicsHandleInformation * Handle = GetPhysicsGrip(Grip);

	if (!Handle)
	{
		PrimComp->SetWorldTransform(WorldTransform, false, NULL, ETeleportType::TeleportPhysics);
	}
	else if (Handle && Handle->KinActorData2.IsValid() && bTeleportPhysicsGrips)
	{
		// Don't try to autodrop on next tick, let the physx constraint update its local frame first
		if (HasGripAuthority(Grip))
			Grip.bSkipNextConstraintLengthCheck = true;

		PrimComp->SetWorldTransform(WorldTransform, false, NULL, ETeleportType::TeleportPhysics);

		FPhysicsCommand::ExecuteWrite(Handle->KinActorData2, [&](const FPhysicsActorHandle& Actor)
		{
			// Zero out our scale now that we are working outside of physx
			physicsTrans.SetScale3D(FVector(1.0f));

			FTransform newTrans = Handle->COMPosition * (Handle->RootBoneRotation * physicsTrans);
			FPhysicsInterface::SetKinematicTarget_AssumesLocked(Actor, newTrans);
			FPhysicsInterface::SetGlobalPose_AssumesLocked(Actor, newTrans);
		});
	}

	return true;
}

void UGripMotionControllerComponent::PostTeleportMoveGrippedObjects()
{
	if (!GrippedObjects.Num() && !LocallyGrippedObjects.Num())
		return;

	this->bIsPostTeleport = true;
	/*for (int i = 0; i < LocallyGrippedObjects.Num(); i++)
	{
		TeleportMoveGrip(LocallyGrippedObjects[i], true);
	}

	for (int i = 0; i < GrippedObjects.Num(); i++)
	{
		TeleportMoveGrip(GrippedObjects[i], true);
	}*/
}


void UGripMotionControllerComponent::Deactivate()
{
	Super::Deactivate();

	if (bIsActive == false && GripViewExtension.IsValid())
	{
		{
			// This component could be getting accessed from the render thread so it needs to wait
			// before clearing MotionControllerComponent 
			FScopeLock ScopeLock(&CritSect);
			GripViewExtension->MotionControllerComponent = NULL;
		}

		GripViewExtension.Reset();
	}
}


void UGripMotionControllerComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	// Skip motion controller tick, we override a lot of things that it does and we don't want it to perform the same functions
	Super::Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bIsActive)
		return;

	// Moved this here instead of in the polling function, it was ticking once per frame anyway so no loss of perf
	// It doesn't need to be there and now I can pre-check
	// Also epics implementation in the polling function didn't work anyway as it was based off of playercontroller which is not the owner of this controller
	
	// Cache state from the game thread for use on the render thread
	// No need to check if in game thread here as tick always is
	bHasAuthority = IsLocallyControlled();

	// Server/remote clients don't set the controller position in VR
	// Don't call positional checks and don't create the late update scene view
	if (bHasAuthority)
	{
		if (bOffsetByControllerProfile && !NewControllerProfileEvent_Handle.IsValid())
		{
			GetCurrentProfileTransform(true);
		}

		FVector Position = FVector::ZeroVector;
		FRotator Orientation = FRotator::ZeroRotator;

		if (!bUseWithoutTracking)
		{
			if (!GripViewExtension.IsValid() && GEngine)
			{
				GripViewExtension = FSceneViewExtensions::NewExtension<FGripViewExtension>(this);
			}

			float WorldToMeters = GetWorld() ? GetWorld()->GetWorldSettings()->WorldToMeters : 100.0f;
			ETrackingStatus LastTrackingStatus = CurrentTrackingStatus;
			const bool bNewTrackedState = GripPollControllerState(Position, Orientation, WorldToMeters);

			bTracked = bNewTrackedState && CurrentTrackingStatus != ETrackingStatus::NotTracked;
			if (bTracked)
			{
				SetRelativeTransform(FTransform(Orientation, Position, this->RelativeScale3D));
			}

			// if controller tracking just changed
			if (LastTrackingStatus != CurrentTrackingStatus)
			{
				OnTrackingChanged.Broadcast(CurrentTrackingStatus);

				if (LastTrackingStatus == ETrackingStatus::NotTracked)
				{
					// Handle the display component
					// #TODO: Don't run if already has a display model, can't access yet
					if (bDisplayDeviceModel && DisplayModelSource != UMotionControllerComponent::CustomModelSourceId)
						RefreshDisplayComponent();
				}
			}
		}

		if (!bTracked && !bUseWithoutTracking)
			return; // Don't update anything including location

		// Don't bother with any of this if not replicating transform
		if (bReplicates && (bTracked || bReplicateWithoutTracking))
		{
			// Don't rep if no changes
			if (!this->RelativeLocation.Equals(ReplicatedControllerTransform.Position) || !this->RelativeRotation.Equals(ReplicatedControllerTransform.Rotation))
			{
				ControllerNetUpdateCount += DeltaTime;
				if (ControllerNetUpdateCount >= (1.0f / ControllerNetUpdateRate))
				{
					ControllerNetUpdateCount = 0.0f;

					// Tracked doesn't matter, already set the relative location above in that case
					ReplicatedControllerTransform.Position = this->RelativeLocation;
					ReplicatedControllerTransform.Rotation = this->RelativeRotation;

					// I would keep the torn off check here, except this can be checked on tick if they
					// Set 100 htz updates, and in the TornOff case, it actually can't hurt any besides some small
					// Perf difference.
					if (GetNetMode() == NM_Client/* && !IsTornOff()*/)
					{		
						AVRBaseCharacter * OwningChar = Cast<AVRBaseCharacter>(GetOwner());
						if (OverrideSendTransform != nullptr && OwningChar != nullptr)
						{
							(OwningChar->* (OverrideSendTransform))(ReplicatedControllerTransform);
						}
						else
							Server_SendControllerTransform(ReplicatedControllerTransform);
					}
				}
			}
		}
	}
	else
	{
		if (bLerpingPosition)
		{
			ControllerNetUpdateCount += DeltaTime;
			float LerpVal = FMath::Clamp(ControllerNetUpdateCount / (1.0f / ControllerNetUpdateRate), 0.0f, 1.0f);

			if (LerpVal >= 1.0f)
			{
				SetRelativeLocationAndRotation(ReplicatedControllerTransform.Position, ReplicatedControllerTransform.Rotation);

				// Stop lerping, wait for next update if it is delayed or lost then it will hitch here
				// Actual prediction might be something to consider in the future, but rough to do in VR
				// considering the speed and accuracy of movements
				// would like to consider sub stepping but since there is no server rollback...not sure how useful it would be
				// and might be perf taxing enough to not make it worth it.
				bLerpingPosition = false;
				ControllerNetUpdateCount = 0.0f;
			}
			else
			{
				// Removed variables to speed this up a bit
				SetRelativeLocationAndRotation(
					FMath::Lerp(LastUpdatesRelativePosition, (FVector)ReplicatedControllerTransform.Position, LerpVal), 
					FMath::Lerp(LastUpdatesRelativeRotation, ReplicatedControllerTransform.Rotation, LerpVal)
				);
			}
		}
	}

	// Process the gripped actors
	TickGrip(DeltaTime);

}

bool UGripMotionControllerComponent::GetGripWorldTransform(TArray<UVRGripScriptBase*>& GripScripts, float DeltaTime, FTransform & WorldTransform, const FTransform &ParentTransform, FBPActorGripInformation &Grip, AActor * actor, UPrimitiveComponent * root, bool bRootHasInterface, bool bActorHasInterface, bool bIsForTeleport, bool &bForceADrop)
{
	SCOPE_CYCLE_COUNTER(STAT_GetGripTransform);

	bool bHasValidTransform = true;

	if (GripScripts.Num())
	{
		bool bGetDefaultTransform = true;

		// Get grip script world transform overrides (if there are any)
		for (UVRGripScriptBase* Script: GripScripts)
		{
			if (Script && Script->IsScriptActive() && Script->GetWorldTransformOverrideType() == EGSTransformOverrideType::OverridesWorldTransform)
			{
				// One of the grip scripts overrides the default transform
				bGetDefaultTransform = false;
				break;
			}
		}

		// If none of the scripts override the base transform
		if (bGetDefaultTransform && DefaultGripScript)
		{		
			bHasValidTransform = DefaultGripScript->CallCorrect_GetWorldTransform(this, DeltaTime, WorldTransform, ParentTransform, Grip, actor, root, bRootHasInterface, bActorHasInterface, bIsForTeleport);
			bForceADrop = DefaultGripScript->Wants_ToForceDrop();
		}

		// Get grip script world transform modifiers (if there are any)
		for (UVRGripScriptBase* Script : GripScripts)
		{
			if (Script && Script->IsScriptActive() && Script->GetWorldTransformOverrideType() != EGSTransformOverrideType::None)
			{
				bHasValidTransform = Script->CallCorrect_GetWorldTransform(this, DeltaTime, WorldTransform, ParentTransform, Grip, actor, root, bRootHasInterface, bActorHasInterface, bIsForTeleport);
				bForceADrop = Script->Wants_ToForceDrop();

				// Early out, one of the scripts is telling us that the transform isn't valid, something went wrong or the grip is flagged for drop
				if (!bHasValidTransform || bForceADrop)
					break;
			}
		}
	}
	else
	{
		if (DefaultGripScript)
		{
			bHasValidTransform = DefaultGripScript->CallCorrect_GetWorldTransform(this, DeltaTime, WorldTransform, ParentTransform, Grip, actor, root, bRootHasInterface, bActorHasInterface, bIsForTeleport);
			bForceADrop = DefaultGripScript->Wants_ToForceDrop();
		}
	}

	return bHasValidTransform;
}

void UGripMotionControllerComponent::TickGrip(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_TickGrip);

	// Debug test that we aren't floating physics handles
	if (PhysicsGrips.Num() > (GrippedObjects.Num() + LocallyGrippedObjects.Num()))
	{
		CleanUpBadPhysicsHandles();
		UE_LOG(LogVRMotionController, Warning, TEXT("Something went wrong, there were too many physics handles for how many grips exist! Cleaned up bad handles."));
	}
	//check(PhysicsGrips.Num() <= (GrippedObjects.Num() + LocallyGrippedObjects.Num()));

	FTransform ParentTransform = GetPivotTransform();

	// Split into separate functions so that I didn't have to combine arrays since I have some removal going on
	HandleGripArray(GrippedObjects, ParentTransform, DeltaTime, true);
	HandleGripArray(LocallyGrippedObjects, ParentTransform, DeltaTime);

	// Empty out the teleport flag
	bIsPostTeleport = false;

	// Save out the component velocity from this and last frame
	if(!LastRelativePosition.GetTranslation().IsZero())
		ComponentVelocity = (RelativeLocation - LastRelativePosition.GetTranslation()) / DeltaTime;

	// #TODO:
	// Relative angular velocity too?
	// Maybe add some running averaging here to make it work across frames?
	// Or Valves 30 frame high point average buffer
	LastRelativePosition = this->GetRelativeTransform();
}

void UGripMotionControllerComponent::HandleGripArray(TArray<FBPActorGripInformation> &GrippedObjectsArray, const FTransform & ParentTransform, float DeltaTime, bool bReplicatedArray)
{
	if (GrippedObjectsArray.Num())
	{
		FTransform WorldTransform;

		for (int i = GrippedObjectsArray.Num() - 1; i >= 0; --i)
		{
			if (!HasGripMovementAuthority(GrippedObjectsArray[i]))
				continue;

			FBPActorGripInformation * Grip = &GrippedObjectsArray[i];

			if (!Grip) // Shouldn't be possible, but why not play it safe
				continue;

			// Double checking here for a failed rep due to out of order replication from a spawned actor
			if (!Grip->ValueCache.bWasInitiallyRepped && !HasGripAuthority(*Grip) && !HandleGripReplication(*Grip))
				continue; // If we didn't successfully handle the replication (out of order) then continue on.

			// Continue if the grip is paused
			if (Grip->bIsPaused)
				continue;

			if (Grip->GripID != INVALID_VRGRIP_ID && Grip->GrippedObject && !Grip->GrippedObject->IsPendingKill())
			{
				if (Grip->GripCollisionType == EGripCollisionType::EventsOnly)
					continue; // Earliest safe spot to continue at, we needed to check if the object is pending kill or invalid first

				UPrimitiveComponent *root = NULL;
				AActor *actor = NULL;

				// Getting the correct variables depending on the grip target type
				switch (Grip->GripTargetType)
				{
					case EGripTargetType::ActorGrip:
					//case EGripTargetType::InteractibleActorGrip:
					{
						actor = Grip->GetGrippedActor();
						if(actor)
							root = Cast<UPrimitiveComponent>(actor->GetRootComponent());
					}break;

					case EGripTargetType::ComponentGrip:
					//case EGripTargetType::InteractibleComponentGrip :
					{
						root = Grip->GetGrippedComponent();
						if(root)
							actor = root->GetOwner();
					}break;

				default:break;
				}

				// Last check to make sure the variables are valid
				if (!root || !actor)
					continue;

				// Check if either implements the interface
				bool bRootHasInterface = false;
				bool bActorHasInterface = false;
				
				if (root->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
				{
					bRootHasInterface = true;
				}
				if (actor->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
				{
					// Actor grip interface is checked after component
					bActorHasInterface = true;
				}

				if (Grip->GripCollisionType == EGripCollisionType::CustomGrip)
				{
					// Don't perform logic on the movement for this object, just pass in the GripTick() event with the controller difference instead
					if(bRootHasInterface)
						IVRGripInterface::Execute_TickGrip(root, this, *Grip, DeltaTime);
					else if(bActorHasInterface)
						IVRGripInterface::Execute_TickGrip(actor, this, *Grip, DeltaTime);

					continue;
				}

				bool bRescalePhysicsGrips = false;
				
				TArray<UVRGripScriptBase*> GripScripts;

				if (bRootHasInterface)
				{
					IVRGripInterface::Execute_GetGripScripts(root, GripScripts);
				}
				else if (bActorHasInterface)
				{
					IVRGripInterface::Execute_GetGripScripts(actor, GripScripts);
				}


				bool bForceADrop = false;

				// Get the world transform for this grip after handling secondary grips and interaction differences
				bool bHasValidWorldTransform = GetGripWorldTransform(GripScripts, DeltaTime, WorldTransform, ParentTransform, *Grip, actor, root, bRootHasInterface, bActorHasInterface, false, bForceADrop);

				// If a script or behavior is telling us to skip this and continue on (IE: it dropped the grip)
				if (bForceADrop)
				{
					if (HasGripAuthority(*Grip))
					{
						if (bRootHasInterface)
							DropGrip(*Grip, IVRGripInterface::Execute_SimulateOnDrop(root));
						else if (bActorHasInterface)
							DropGrip(*Grip, IVRGripInterface::Execute_SimulateOnDrop(actor));
						else
							DropGrip(*Grip, true);
					}

					continue;
				}
				else if (!bHasValidWorldTransform)
				{
					continue;
				}

				if (!root->GetComponentScale().Equals(WorldTransform.GetScale3D()))
					bRescalePhysicsGrips = true;

				// If we just teleported, skip this update and just teleport forward
				if (bIsPostTeleport)
				{
					TeleportMoveGrip_Impl(*Grip, true, true, WorldTransform);
					Grip->LastWorldTransform = WorldTransform;
					continue;
				}
				else
				{
					Grip->LastWorldTransform = WorldTransform;
				}

				// Auto drop based on distance from expected point
				// Not perfect, should be done post physics or in next frame prior to changing controller location
				// However I don't want to recalculate world transform
				// Maybe add a grip variable of "expected loc" and use that to check next frame, but for now this will do.
				if ((bRootHasInterface || bActorHasInterface) &&
					(
							(Grip->GripCollisionType != EGripCollisionType::AttachmentGrip) &&
							(Grip->GripCollisionType != EGripCollisionType::PhysicsOnly) && 
							(Grip->GripCollisionType != EGripCollisionType::SweepWithPhysics)) &&
							((Grip->GripCollisionType != EGripCollisionType::InteractiveHybridCollisionWithSweep) || ((Grip->GripCollisionType == EGripCollisionType::InteractiveHybridCollisionWithSweep) && Grip->bColliding))
					)
				{

					// After initial teleportation the constraint local pose can be not updated yet, so lets delay a frame to let it update
					// Otherwise may cause unintended auto drops
					if (Grip->bSkipNextConstraintLengthCheck)
					{
						Grip->bSkipNextConstraintLengthCheck = false;
					}
					else
					{
						float BreakDistance = 0.0f;
						if (bRootHasInterface)
						{
							BreakDistance = IVRGripInterface::Execute_GripBreakDistance(root);
						}
						else if (bActorHasInterface)
						{
							// Actor grip interface is checked after component
							BreakDistance = IVRGripInterface::Execute_GripBreakDistance(actor);
						}

						FVector CheckDistance;
						if (!GetPhysicsJointLength(*Grip, root, CheckDistance))
						{
							CheckDistance = (WorldTransform.GetLocation() - root->GetComponentLocation());
						}

						// Set grip distance now for people to use
						Grip->GripDistance = CheckDistance.Size();

						if (BreakDistance > 0.0f)
						{
							if (Grip->GripDistance >= BreakDistance)
							{
								bool bIgnoreDrop = false;
								for (UVRGripScriptBase* Script : GripScripts)
								{
									if (Script && Script->IsScriptActive() && Script->Wants_DenyAutoDrop())
									{
										bIgnoreDrop = true;
										break;
									}
								}

								if (bIgnoreDrop)
								{
									// Script canceled this out
								}
								else if (OnGripOutOfRange.IsBound())
								{
									uint8 GripID = Grip->GripID;
									OnGripOutOfRange.Broadcast(*Grip, Grip->GripDistance);

									// Check if we still have the grip or not
									FBPActorGripInformation GripInfo;
									EBPVRResultSwitch Result;
									GetGripByID(GripInfo, GripID, Result);
									if (Result == EBPVRResultSwitch::OnFailed)
									{
										// Don't bother moving it, it is dropped now
										continue;
									}
								}
								else if(HasGripAuthority(*Grip))
								{
									if(bRootHasInterface)
										DropGrip(*Grip, IVRGripInterface::Execute_SimulateOnDrop(root));
									else
										DropGrip(*Grip, IVRGripInterface::Execute_SimulateOnDrop(actor));

									// Don't bother moving it, it is dropped now
									continue;
								}
							}
						}
					}
				}

				// Start handling the grip types and their functions
				switch (Grip->GripCollisionType)
				{
					case EGripCollisionType::InteractiveCollisionWithPhysics:
					{
						UpdatePhysicsHandleTransform(*Grip, WorldTransform);
						
						if (bRescalePhysicsGrips)
							root->SetWorldScale3D(WorldTransform.GetScale3D());


						// Sweep current collision state, only used for client side late update removal
						if (
							(bHasAuthority &&
								((Grip->GripLateUpdateSetting == EGripLateUpdateSettings::NotWhenColliding) ||
									(Grip->GripLateUpdateSetting == EGripLateUpdateSettings::NotWhenCollidingOrDoubleGripping)))
							)
						{
							//TArray<FOverlapResult> Hits;
							FComponentQueryParams Params(NAME_None, this->GetOwner());
							//Params.bTraceAsyncScene = root->bCheckAsyncSceneOnMove;
							Params.AddIgnoredActor(actor);
							Params.AddIgnoredActors(root->MoveIgnoreActors);

							TArray<FHitResult> Hits;
							
							// Switched over to component sweep because it picks up on pivot offsets without me manually calculating it
							if (GetWorld()->ComponentSweepMulti(Hits, root, root->GetComponentLocation(), WorldTransform.GetLocation(), WorldTransform.GetRotation(), Params))
							{
								Grip->bColliding = true;
							}
							else
							{
								Grip->bColliding = false;
							}
						}

					}break;

					case EGripCollisionType::InteractiveCollisionWithSweep:
					{
						FVector OriginalPosition(root->GetComponentLocation());
						FVector NewPosition(WorldTransform.GetTranslation());

						if (!Grip->bIsLocked)
							root->ComponentVelocity = (NewPosition - OriginalPosition) / DeltaTime;

						if (Grip->bIsLocked)
							WorldTransform.SetRotation(Grip->LastLockedRotation);

						FHitResult OutHit;
						// Need to use without teleport so that the physics velocity is updated for when the actor is released to throw

						root->SetWorldTransform(WorldTransform, true, &OutHit);

						if (OutHit.bBlockingHit)
						{
							Grip->bColliding = true;

							if (!Grip->bIsLocked)
							{
								Grip->bIsLocked = true;
								Grip->LastLockedRotation = root->GetComponentQuat();
							}
						}
						else
						{
							Grip->bColliding = false;

							if (Grip->bIsLocked)
								Grip->bIsLocked = false;
						}
					}break;

					case EGripCollisionType::InteractiveHybridCollisionWithPhysics:
					{
						UpdatePhysicsHandleTransform(*Grip, WorldTransform);

						if (bRescalePhysicsGrips)
							root->SetWorldScale3D(WorldTransform.GetScale3D());

						// Always Sweep current collision state with this, used for constraint strength
						//TArray<FOverlapResult> Hits;
						FComponentQueryParams Params(NAME_None, this->GetOwner());
						//Params.bTraceAsyncScene = root->bCheckAsyncSceneOnMove;
						Params.AddIgnoredActor(actor);
						Params.AddIgnoredActors(root->MoveIgnoreActors);

						TArray<FHitResult> Hits;
						// Checking both current and next position for overlap using this grip type
						// Switched over to component sweep because it picks up on pivot offsets without me manually calculating it
						if (GetWorld()->ComponentSweepMulti(Hits, root, root->GetComponentLocation(), WorldTransform.GetLocation(), WorldTransform.GetRotation(), Params))
						{
							if (!Grip->bColliding)
							{
								SetGripConstraintStiffnessAndDamping(Grip, false);
							}
							Grip->bColliding = true;
						}
						else
						{
							if (Grip->bColliding)
							{
								SetGripConstraintStiffnessAndDamping(Grip, true);
							}

							Grip->bColliding = false;
						}

					}break;

					case EGripCollisionType::InteractiveHybridCollisionWithSweep:
					{

						// Make sure that there is no collision on course before turning off collision and snapping to controller
						FBPActorPhysicsHandleInformation * GripHandle = GetPhysicsGrip(*Grip);

						TArray<FHitResult> Hits;
						FComponentQueryParams Params(NAME_None, this->GetOwner());
						//Params.bTraceAsyncScene = root->bCheckAsyncSceneOnMove;
						Params.AddIgnoredActor(actor);
						Params.AddIgnoredActors(root->MoveIgnoreActors);

						if (GetWorld()->ComponentSweepMulti(Hits, root, root->GetComponentLocation(), WorldTransform.GetLocation(), WorldTransform.GetRotation(), Params))
						{
							Grip->bColliding = true;
						}
						else
						{
							Grip->bColliding = false;
						}

						if (!Grip->bColliding)
						{
							if (GripHandle)
							{
								DestroyPhysicsHandle(*Grip);

								switch (Grip->GripTargetType)
								{
								case EGripTargetType::ComponentGrip:
								{
									root->SetSimulatePhysics(false);
								}break;
								case EGripTargetType::ActorGrip:
								{
									actor->DisableComponentsSimulatePhysics();
								} break;
								}
							}

							root->SetWorldTransform(WorldTransform, false);// , &OutHit);

						}
						else if (Grip->bColliding && !GripHandle)
						{
							root->SetSimulatePhysics(true);

							SetUpPhysicsHandle(*Grip);
							UpdatePhysicsHandleTransform(*Grip, WorldTransform);
							if (bRescalePhysicsGrips)
								root->SetWorldScale3D(WorldTransform.GetScale3D());
						}
						else
						{
							// Shouldn't be a grip handle if not server when server side moving
							if (GripHandle)
							{
								UpdatePhysicsHandleTransform(*Grip, WorldTransform);
								if (bRescalePhysicsGrips)
										root->SetWorldScale3D(WorldTransform.GetScale3D());
							}
						}

					}break;

					case EGripCollisionType::SweepWithPhysics:
					{
						FVector OriginalPosition(root->GetComponentLocation());
						FRotator OriginalOrientation(root->GetComponentRotation());

						FVector NewPosition(WorldTransform.GetTranslation());
						FRotator NewOrientation(WorldTransform.GetRotation());

						root->ComponentVelocity = (NewPosition - OriginalPosition) / DeltaTime;

						// Now sweep collision separately so we can get hits but not have the location altered
						if (bUseWithoutTracking || NewPosition != OriginalPosition || NewOrientation != OriginalOrientation)
						{
							FVector move = NewPosition - OriginalPosition;

							// ComponentSweepMulti does nothing if moving < KINDA_SMALL_NUMBER in distance, so it's important to not try to sweep distances smaller than that. 
							const float MinMovementDistSq = (FMath::Square(4.f*KINDA_SMALL_NUMBER));

							if (bUseWithoutTracking || move.SizeSquared() > MinMovementDistSq || NewOrientation != OriginalOrientation)
							{
								if (CheckComponentWithSweep(root, move, OriginalOrientation, false))
								{
									Grip->bColliding = true;
								}
								else
								{
									Grip->bColliding = false;
								}

								TArray<USceneComponent* > PrimChildren;
								root->GetChildrenComponents(true, PrimChildren);
								for (USceneComponent * Prim : PrimChildren)
								{
									if (UPrimitiveComponent * primComp = Cast<UPrimitiveComponent>(Prim))
									{
										CheckComponentWithSweep(primComp, move, primComp->GetComponentRotation(), false);
									}
								}
							}
						}

						// Move the actor, we are not offsetting by the hit result anyway
						root->SetWorldTransform(WorldTransform, false);

					}break;

					case EGripCollisionType::PhysicsOnly:
					{
						// Move the actor, we are not offsetting by the hit result anyway
						root->SetWorldTransform(WorldTransform, false);
					}break;

					case EGripCollisionType::AttachmentGrip:
					{
						FTransform RelativeTrans = WorldTransform.GetRelativeTransform(ParentTransform);
						if (!root->GetRelativeTransform().Equals(RelativeTrans))
						{
							root->SetRelativeTransform(RelativeTrans);
						}

					}break;

					case EGripCollisionType::ManipulationGrip:
					case EGripCollisionType::ManipulationGripWithWristTwist:
					{
						UpdatePhysicsHandleTransform(*Grip, WorldTransform);
						if (bRescalePhysicsGrips)
							root->SetWorldScale3D(WorldTransform.GetScale3D());

					}break;

					default:
					{}break;
				}

				// We only do this if specifically requested, it has a slight perf hit and isn't normally needed for non Custom Grip types
				if (bAlwaysSendTickGrip)
				{
					// All non custom grips tick after translation, this is still pre physics so interactive grips location will be wrong, but others will be correct
					if (bRootHasInterface)
					{
						IVRGripInterface::Execute_TickGrip(root, this, *Grip, DeltaTime);
					}

					if (bActorHasInterface)
					{
						IVRGripInterface::Execute_TickGrip(actor, this, *Grip, DeltaTime);
					}
				}
			}
			else
			{
				// Object has been destroyed without notification to plugin
				CleanUpBadGrip(GrippedObjectsArray, i, bReplicatedArray);
			}
		}
	}
}


void UGripMotionControllerComponent::CleanUpBadGrip(TArray<FBPActorGripInformation> &GrippedObjectsArray, int GripIndex, bool bReplicatedArray)
{
	// Object has been destroyed without notification to plugin
	if (!DestroyPhysicsHandle(GrippedObjectsArray[GripIndex]))
	{
		// Clean up tailing physics handles with null objects
		for (int g = PhysicsGrips.Num() - 1; g >= 0; --g)
		{
			if (!PhysicsGrips[g].HandledObject || PhysicsGrips[g].HandledObject == GrippedObjectsArray[GripIndex].GrippedObject || PhysicsGrips[g].HandledObject->IsPendingKill())
			{
				// Need to delete it from the physics thread
				DestroyPhysicsHandle(&PhysicsGrips[g]);
				PhysicsGrips.RemoveAt(g);
			}
		}
	}

	if (HasGripAuthority(GrippedObjectsArray[GripIndex]))
	{
		DropGrip(GrippedObjectsArray[GripIndex], false);
		UE_LOG(LogVRMotionController, Warning, TEXT("Gripped object was null or destroying, auto dropping it"));
	}
	else
	{
		GrippedObjectsArray[GripIndex].bIsPaused = true;
	}
}

void UGripMotionControllerComponent::CleanUpBadPhysicsHandles()
{
	// Clean up tailing physics handles with null objects
	for (int g = PhysicsGrips.Num() - 1; g >= 0; --g)
	{
		FBPActorGripInformation * GripInfo = LocallyGrippedObjects.FindByKey(PhysicsGrips[g].GripID);
		if(!GripInfo)
			GrippedObjects.FindByKey(PhysicsGrips[g].GripID);

		if (!GripInfo)
		{
			// Need to delete it from the physics thread
			DestroyPhysicsHandle(&PhysicsGrips[g]);
			PhysicsGrips.RemoveAt(g);
		}
	}
}

bool UGripMotionControllerComponent::UpdatePhysicsHandle(uint8 GripID, bool bFullyRecreate)
{
	FBPActorGripInformation* GripInfo = GrippedObjects.FindByKey(GripID);
	if (!GripInfo)
		GripInfo = LocallyGrippedObjects.FindByKey(GripID);

	if (!GripInfo)
		return false;

	return UpdatePhysicsHandle(*GripInfo, bFullyRecreate);
}

bool UGripMotionControllerComponent::UpdatePhysicsHandle(const FBPActorGripInformation& GripInfo, bool bFullyRecreate)
{
	int HandleIndex = 0;
	bool bHadPhysicsHandle = GetPhysicsGripIndex(GripInfo, HandleIndex);

	if (!bHadPhysicsHandle)
		return false;

	if (bFullyRecreate)
	{
		return SetUpPhysicsHandle(GripInfo);
	}

	// Not fully recreating
#if WITH_PHYSX

	UPrimitiveComponent* root = GripInfo.GetGrippedComponent();
	AActor* pActor = GripInfo.GetGrippedActor();

	if (!root && pActor)
		root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

	if (!root)
		return false;

	FBodyInstance* rBodyInstance = root->GetBodyInstance(GripInfo.GrippedBoneName);
	if (!rBodyInstance || !rBodyInstance->IsValidBodyInstance())
	{
		return false;
	}

	check(rBodyInstance->BodySetup->GetCollisionTraceFlag() != CTF_UseComplexAsSimple);

	FBPActorPhysicsHandleInformation* HandleInfo = &PhysicsGrips[HandleIndex];
	FPhysicsCommand::ExecuteWrite(rBodyInstance->ActorHandle, [&](const FPhysicsActorHandle& Actor)
		{
			if (PxRigidDynamic * PActor = FPhysicsInterface::GetPxRigidDynamic_AssumesLocked(Actor))
			{
				HandleInfo->HandleData2.ConstraintData->setActors(FPhysicsInterface::GetPxRigidDynamic_AssumesLocked(HandleInfo->KinActorData2), PActor);
			}

			if (HandleInfo->bSetCOM)
			{
				FVector Loc = (FTransform((HandleInfo->RootBoneRotation * GripInfo.RelativeTransform).ToInverseMatrixWithScale())).GetLocation();
				Loc *= rBodyInstance->Scale3D;

				FTransform localCom = FPhysicsInterface::GetComTransformLocal_AssumesLocked(Actor);
				localCom.SetLocation(Loc);

				FPhysicsInterface::SetComLocalPose_AssumesLocked(Actor, localCom);
			}

		});

	return true;
#endif

	return false;
}

bool UGripMotionControllerComponent::DestroyPhysicsHandle(FBPActorPhysicsHandleInformation* HandleInfo)
{
	if (!HandleInfo)
		return false;

	FPhysicsInterface::ReleaseConstraint(HandleInfo->HandleData2);
	FPhysicsInterface::ReleaseActor(HandleInfo->KinActorData2, FPhysicsInterface::GetCurrentScene(HandleInfo->KinActorData2));

	return true;
}

bool UGripMotionControllerComponent::DestroyPhysicsHandle(const FBPActorGripInformation &Grip, bool bSkipUnregistering)
{
	FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(Grip);

	if (!HandleInfo)
	{
		return true;
	}

	UPrimitiveComponent *root = Grip.GetGrippedComponent();
	AActor * pActor = Grip.GetGrippedActor();

	if (!root && pActor)
		root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

	if (root)
	{
		if (FBodyInstance * rBodyInstance = root->GetBodyInstance(Grip.GrippedBoneName))
		{
			// #TODO: Should this be done on drop instead?
			// Remove event registration
			if (!bSkipUnregistering)
			{
				if (rBodyInstance->OnRecalculatedMassProperties.IsBoundToObject(this))
				{
					rBodyInstance->OnRecalculatedMassProperties.RemoveAll(this);
				}
			}

			if (HandleInfo->bSetCOM)
			{
				// Reset center of mass to zero
				// Get our original values
				FVector vel = rBodyInstance->GetUnrealWorldVelocity();
				FVector aVel = rBodyInstance->GetUnrealWorldAngularVelocityInRadians();
				FVector originalCOM = rBodyInstance->GetCOMPosition();

				rBodyInstance->UpdateMassProperties();

				// Offset the linear velocity by the new COM position and set it
				vel += FVector::CrossProduct(aVel, rBodyInstance->GetCOMPosition() - originalCOM);
				rBodyInstance->SetLinearVelocity(vel, false);
			}
		}
	}

	DestroyPhysicsHandle(HandleInfo);

	int index;
	if (GetPhysicsGripIndex(Grip, index))
		PhysicsGrips.RemoveAt(index);

	return true;
}

void UGripMotionControllerComponent::OnGripMassUpdated(FBodyInstance* GripBodyInstance)
{
	TArray<FBPActorGripInformation> GripArray;
	this->GetAllGrips(GripArray);
	FBPActorGripInformation NewGrip;

	for (int i = 0; i < GripArray.Num(); i++)
	{
		NewGrip = GripArray[i];

		UPrimitiveComponent *root = NewGrip.GetGrippedComponent();
		AActor * pActor = NewGrip.GetGrippedActor();

		if (!root && pActor)
			root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());

		if (!root || root != GripBodyInstance->OwnerComponent)
			continue;

		UpdatePhysicsHandle(NewGrip);
		break;
	}
}

bool UGripMotionControllerComponent::SetUpPhysicsHandle(const FBPActorGripInformation &NewGrip)
{
	UPrimitiveComponent *root = NewGrip.GetGrippedComponent();
	AActor * pActor = NewGrip.GetGrippedActor();

	if(!root && pActor)
		root = Cast<UPrimitiveComponent>(pActor->GetRootComponent());
	
	if (!root)
		return false;

	FBPActorPhysicsHandleInformation* HandleInfo = GetPhysicsGrip(NewGrip);
	if (HandleInfo == nullptr)
	{
		HandleInfo = CreatePhysicsGrip(NewGrip);
	}

	// Needs to be simulating in order to run physics
	root->SetSimulatePhysics(true);
	/*if(NewGrip.GrippedBoneName == NAME_None)
		root->SetSimulatePhysics(true);
	else
	{
		USkeletalMeshComponent * skele = Cast<USkeletalMeshComponent>(root);
		if (skele)
			skele->SetAllBodiesBelowSimulatePhysics(NewGrip.GrippedBoneName, true);
	}*/

	// Get the PxRigidDynamic that we want to grab.
	FBodyInstance* rBodyInstance = root->GetBodyInstance(NewGrip.GrippedBoneName);
	if (!rBodyInstance || !rBodyInstance->IsValidBodyInstance() || !rBodyInstance->ActorHandle.IsValid())
	{	
		return false;
	}


	check(rBodyInstance->BodySetup->GetCollisionTraceFlag() != CTF_UseComplexAsSimple);
	
	if (!HandleInfo->KinActorData2.IsValid())
	{
		// Reset the mass properties, this avoids an issue with some weird replication issues
		// We only do this on initial grip
		rBodyInstance->UpdateMassProperties();
	}

	/*if (NewGrip.GrippedBoneName != NAME_None)
	{
		rBodyInstance->SetInstanceSimulatePhysics(true);
	}*/

	FPhysicsCommand::ExecuteWrite(rBodyInstance->ActorHandle, [&](const FPhysicsActorHandle& Actor)
	{

		FTransform KinPose;
		FTransform trans = FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor);
		FTransform RootBoneRotation = FTransform::Identity;

		if (NewGrip.GrippedBoneName != NAME_None)
		{
			// Skip root bone rotation
		}
		else
		{
			// I actually don't need any of this code anymore or the HandleInfo->RootBoneRotation
			// However I would have to expect people to pass in the bone transform without it.
			// For now I am keeping it to keep it backwards compatible as it will adjust for root bone rotation automatically then
			if (USkeletalMeshComponent * skele = Cast<USkeletalMeshComponent>(root))
			{
				int32 RootBodyIndex = INDEX_NONE;
				if (const UPhysicsAsset* PhysicsAsset = skele->GetPhysicsAsset())
				{
					for (int32 i = 0; i < skele->GetNumBones(); i++)
					{
						if (PhysicsAsset->FindBodyIndex(skele->GetBoneName(i)) != INDEX_NONE)
						{
							RootBodyIndex = i;
							break;
						}
					}
				}

				if (RootBodyIndex != INDEX_NONE)
				{
					RootBoneRotation = FTransform(skele->GetBoneTransform(RootBodyIndex, FTransform::Identity));
					HandleInfo->RootBoneRotation = RootBoneRotation;
				}
			}
		}

		EPhysicsGripCOMType COMType = NewGrip.AdvancedGripSettings.PhysicsSettings.PhysicsGripLocationSettings;

		if (!NewGrip.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings || COMType == EPhysicsGripCOMType::COM_Default)
		{
			if (NewGrip.GripCollisionType == EGripCollisionType::ManipulationGrip || NewGrip.GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
			{
				COMType = EPhysicsGripCOMType::COM_GripAtControllerLoc;
			}
			else
			{
				COMType = EPhysicsGripCOMType::COM_SetAndGripAt;
			}
		}

		if (COMType == EPhysicsGripCOMType::COM_SetAndGripAt)
		{			
			// Update the center of mass
			FVector Loc = (FTransform((RootBoneRotation * NewGrip.RelativeTransform).ToInverseMatrixWithScale())).GetLocation();
			Loc *= rBodyInstance->Scale3D;

			FTransform localCom = FPhysicsInterface::GetComTransformLocal_AssumesLocked(Actor);
			localCom.SetLocation(Loc);
			FPhysicsInterface::SetComLocalPose_AssumesLocked(Actor, localCom);
				
			trans.SetLocation(FPhysicsInterface::GetComTransform_AssumesLocked(Actor).GetLocation());
			HandleInfo->bSetCOM = true;
		}

		if (COMType == EPhysicsGripCOMType::COM_GripAtControllerLoc)
		{
			FVector ControllerLoc = (FTransform(NewGrip.RelativeTransform.ToInverseMatrixWithScale()) * root->GetComponentTransform()).GetLocation();
			trans.SetLocation(ControllerLoc);
			HandleInfo->COMPosition = FTransform(rBodyInstance->GetUnrealWorldTransform().InverseTransformPosition(ControllerLoc));
		}
		else if (COMType != EPhysicsGripCOMType::COM_AtPivot)
		{
			FVector ComLoc = FPhysicsInterface::GetComTransform_AssumesLocked(Actor).GetLocation();
			trans.SetLocation(ComLoc);
			HandleInfo->COMPosition = FTransform(rBodyInstance->GetUnrealWorldTransform().InverseTransformPosition(ComLoc));
		}

		KinPose = trans;
		bool bRecreatingConstraint = false;

		if (!HandleInfo->KinActorData2.IsValid())
		{
			// Create kinematic actor we are going to create joint with. This will be moved around with calls to SetLocation/SetRotation.
				
			//FString DebugName(TEXT("KinematicGripActor"));
			//TSharedPtr<TArray<ANSICHAR>> PhysXName = MakeShareable(new TArray<ANSICHAR>(StringToArray<ANSICHAR>(*DebugName, DebugName.Len() + 1)));
				
			FActorCreationParams ActorParams;
			ActorParams.InitialTM = KinPose;
			ActorParams.DebugName = nullptr;//PhysXName->GetData();
			ActorParams.bEnableGravity = false;
			ActorParams.bQueryOnly = false;// true; // True or false?
			ActorParams.bStatic = false;
			ActorParams.Scene = FPhysicsInterface::GetCurrentScene(Actor);
			HandleInfo->KinActorData2 = FPhysicsInterface::CreateActor(ActorParams);
			
			if (HandleInfo->KinActorData2.IsValid())
			{
				FPhysicsInterface::SetMass_AssumesLocked(HandleInfo->KinActorData2, 1.0f);
				FPhysicsInterface::SetMassSpaceInertiaTensor_AssumesLocked(HandleInfo->KinActorData2, FVector(1.f));
				FPhysicsInterface::SetIsKinematic_AssumesLocked(HandleInfo->KinActorData2, true);
				FPhysicsInterface::SetMaxDepenetrationVelocity_AssumesLocked(HandleInfo->KinActorData2, MAX_FLT);
				//FPhysicsInterface::SetActorUserData_AssumesLocked(HandleInfo->KinActorData2, NULL);
			}

#if WITH_PHYSX
			// Correct method is missing an ENGINE_API flag, so I can't use the function
			ActorParams.Scene->GetPxScene()->addActor(*FPhysicsInterface_PhysX::GetPxRigidActor_AssumesLocked(HandleInfo->KinActorData2));
#else
			// Missing from physx, not sure how it is working for them currently.
			TArray<FPhysicsActorHandle> ActorHandles;
			ActorHandles.Add(HandleInfo->KinActorData2);
			ActorParams.Scene->AddActorsToScene_AssumesLocked(ActorHandles);
#endif
		}

		// If we don't already have a handle - make one now.
		if (!HandleInfo->HandleData2.IsValid())
		{
			HandleInfo->HandleData2 = FPhysicsInterface::CreateConstraint(Actor, HandleInfo->KinActorData2, KinPose.GetRelativeTransform(FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor)), FTransform::Identity);
		}
		else
		{
			bRecreatingConstraint = true;

#if WITH_PHYSX
			HandleInfo->HandleData2.ConstraintData->setActors(FPhysicsInterface_PhysX::GetPxRigidDynamic_AssumesLocked(HandleInfo->KinActorData2), FPhysicsInterface_PhysX::GetPxRigidDynamic_AssumesLocked(Actor));
#endif
			
			FPhysicsInterface::SetLocalPose(HandleInfo->HandleData2, KinPose.GetRelativeTransform(FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor)), EConstraintFrame::Frame1);
		}

		if (HandleInfo->HandleData2.IsValid())
		{
			FPhysicsInterface::SetBreakForces_AssumesLocked(HandleInfo->HandleData2, MAX_FLT, MAX_FLT);

			FPhysicsInterface::SetLinearMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::X, ELinearConstraintMotion::LCM_Free);
			FPhysicsInterface::SetLinearMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::Y, ELinearConstraintMotion::LCM_Free);
			FPhysicsInterface::SetLinearMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::Z, ELinearConstraintMotion::LCM_Free);
			FPhysicsInterface::SetAngularMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::Twist, EAngularConstraintMotion::ACM_Free);
			FPhysicsInterface::SetAngularMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::Swing1, EAngularConstraintMotion::ACM_Free);
			FPhysicsInterface::SetAngularMotionLimitType_AssumesLocked(HandleInfo->HandleData2, PhysicsInterfaceTypes::ELimitAxis::Swing2, EAngularConstraintMotion::ACM_Free);

			FPhysicsInterface::SetDrivePosition(HandleInfo->HandleData2, FVector::ZeroVector);

			bool bUseForceDrive = (NewGrip.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewGrip.AdvancedGripSettings.PhysicsSettings.PhysicsConstraintType == EPhysicsGripConstraintType::ForceConstraint);

			float Stiffness = NewGrip.Stiffness;
			float Damping = NewGrip.Damping;
			float AngularStiffness;
			float AngularDamping;

			if (NewGrip.AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && NewGrip.AdvancedGripSettings.PhysicsSettings.bUseCustomAngularValues)
			{
				AngularStiffness = NewGrip.AdvancedGripSettings.PhysicsSettings.AngularStiffness;
				AngularDamping = NewGrip.AdvancedGripSettings.PhysicsSettings.AngularDamping;
			}
			else
			{
				AngularStiffness = Stiffness * ANGULAR_STIFFNESS_MULTIPLIER; // Default multiplier
				AngularDamping = Damping * ANGULAR_DAMPING_MULTIPLIER; // Default multiplier
			}


			// Different settings for manip grip
			if (NewGrip.GripCollisionType == EGripCollisionType::ManipulationGrip || NewGrip.GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
			{
				if (!bRecreatingConstraint)
				{
					FConstraintDrive NewLinDrive;
					NewLinDrive.bEnablePositionDrive = true;
					NewLinDrive.bEnableVelocityDrive = true;
					NewLinDrive.Damping = Damping;
					NewLinDrive.Stiffness = Stiffness;
					NewLinDrive.MaxForce = MAX_FLT;

					HandleInfo->LinConstraint.bEnablePositionDrive = true;
					HandleInfo->LinConstraint.XDrive = NewLinDrive;
					HandleInfo->LinConstraint.YDrive = NewLinDrive;
					HandleInfo->LinConstraint.ZDrive = NewLinDrive;
				}

				FPhysicsInterface::UpdateLinearDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->LinConstraint);

				if (NewGrip.GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
				{
					if (!bRecreatingConstraint)
					{
						FConstraintDrive NewAngDrive;
						NewAngDrive.bEnablePositionDrive = true;
						NewAngDrive.bEnableVelocityDrive = true;
						NewAngDrive.Damping = AngularDamping;
						NewAngDrive.Stiffness = AngularStiffness;
						NewAngDrive.MaxForce = MAX_FLT;

						HandleInfo->AngConstraint.AngularDriveMode = EAngularDriveMode::TwistAndSwing;
						//AngParams.AngularDriveMode = EAngularDriveMode::SLERP;
						HandleInfo->AngConstraint.TwistDrive = NewAngDrive;
					}

					FPhysicsInterface::UpdateAngularDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->AngConstraint);
				}
			}
			else
			{
				if (NewGrip.GripCollisionType == EGripCollisionType::InteractiveHybridCollisionWithPhysics)
				{
					// Do not effect damping, just increase stiffness so that it is stronger
					// Default multiplier
					Stiffness *= HYBRID_PHYSICS_GRIP_MULTIPLIER;
					AngularStiffness *= HYBRID_PHYSICS_GRIP_MULTIPLIER;
				}

				if (!bRecreatingConstraint)
				{
					FConstraintDrive NewLinDrive;
					NewLinDrive.bEnablePositionDrive = true;
					NewLinDrive.bEnableVelocityDrive = true;
					NewLinDrive.Damping = Damping;
					NewLinDrive.Stiffness = Stiffness;
					NewLinDrive.MaxForce = MAX_FLT;

					FConstraintDrive NewAngDrive;
					NewAngDrive.bEnablePositionDrive = true;
					NewAngDrive.bEnableVelocityDrive = true;
					NewAngDrive.Damping = AngularDamping;
					NewAngDrive.Stiffness = AngularStiffness;
					NewAngDrive.MaxForce = MAX_FLT;

					HandleInfo->LinConstraint.bEnablePositionDrive = true;
					HandleInfo->LinConstraint.XDrive = NewLinDrive;
					HandleInfo->LinConstraint.YDrive = NewLinDrive;
					HandleInfo->LinConstraint.ZDrive = NewLinDrive;

					HandleInfo->AngConstraint.AngularDriveMode = EAngularDriveMode::SLERP;
					HandleInfo->AngConstraint.SlerpDrive = NewAngDrive;
				}
					
				FPhysicsInterface::UpdateLinearDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->LinConstraint);
				FPhysicsInterface::UpdateAngularDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->AngConstraint);
			}


			// This is a temp workaround until epic fixes the drive creation to allow force constraints
			// I wanted to use the new interface and not directly set the drive so that it is ready to delete this section
			// When its fixed
			if (bUseForceDrive)
			{
#if WITH_PHYSX
				PxD6JointDrive driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eX);
				driveVal.flags = PxD6JointDriveFlags();//&= ~PxD6JointDriveFlag::eACCELERATION;
				HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eX, driveVal);

				driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eY);
				driveVal.flags = PxD6JointDriveFlags();//&= ~PxD6JointDriveFlag::eACCELERATION;
				HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eY, driveVal);

				driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eZ);
				driveVal.flags = PxD6JointDriveFlags();//&= ~PxD6JointDriveFlag::eACCELERATION;
				HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eZ, driveVal);


				if (NewGrip.GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
				{
					driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eTWIST);
					driveVal.flags = PxD6JointDriveFlags();//&= ~PxD6JointDriveFlag::eACCELERATION;
					HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eTWIST, driveVal);
				}
				else if (NewGrip.GripCollisionType != EGripCollisionType::ManipulationGrip)
				{
					driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eSLERP);
					driveVal.flags = PxD6JointDriveFlags();//&= ~PxD6JointDriveFlag::eACCELERATION;
					HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eSLERP, driveVal);
				}
#endif
			}
		}
	});

	// Bind to further updates in order to keep it alive
	if (!rBodyInstance->OnRecalculatedMassProperties.IsBoundToObject(this))
	{
		rBodyInstance->OnRecalculatedMassProperties.AddUObject(this, &UGripMotionControllerComponent::OnGripMassUpdated);
	}

	return true;
}

bool UGripMotionControllerComponent::SetGripConstraintStiffnessAndDamping(const FBPActorGripInformation *Grip, bool bUseHybridMultiplier)
{
	if (!Grip)
		return false;

	FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(*Grip);

	if (HandleInfo)
	{
		if (HandleInfo->HandleData2.IsValid())
		{

			bool bUseForceDrive = (Grip->AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && Grip->AdvancedGripSettings.PhysicsSettings.PhysicsConstraintType == EPhysicsGripConstraintType::ForceConstraint);

			float Stiffness = Grip->Stiffness;
			float Damping = Grip->Damping;
			float AngularStiffness;
			float AngularDamping;

			if (Grip->AdvancedGripSettings.PhysicsSettings.bUsePhysicsSettings && Grip->AdvancedGripSettings.PhysicsSettings.bUseCustomAngularValues)
			{
				AngularStiffness = Grip->AdvancedGripSettings.PhysicsSettings.AngularStiffness;
				AngularDamping = Grip->AdvancedGripSettings.PhysicsSettings.AngularDamping;
			}
			else
			{
				AngularStiffness = Stiffness * ANGULAR_STIFFNESS_MULTIPLIER; // Default multiplier
				AngularDamping = Damping * ANGULAR_DAMPING_MULTIPLIER; // Default multiplier
			}


			// Different settings for manip grip
			if (Grip->GripCollisionType == EGripCollisionType::ManipulationGrip || Grip->GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
			{
				HandleInfo->LinConstraint.XDrive.Damping = Damping;
				HandleInfo->LinConstraint.XDrive.Stiffness = Stiffness;
				HandleInfo->LinConstraint.YDrive.Damping = Damping;
				HandleInfo->LinConstraint.YDrive.Stiffness = Stiffness;
				HandleInfo->LinConstraint.ZDrive.Damping = Damping;
				HandleInfo->LinConstraint.ZDrive.Stiffness = Stiffness;

				FPhysicsInterface::UpdateLinearDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->LinConstraint);

				if (Grip->GripCollisionType == EGripCollisionType::ManipulationGripWithWristTwist)
				{
					HandleInfo->AngConstraint.TwistDrive.Damping = AngularDamping;
					HandleInfo->AngConstraint.TwistDrive.Stiffness = AngularStiffness;

					FPhysicsInterface::UpdateAngularDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->AngConstraint);

					if (bUseForceDrive)
					{
#if WITH_PHYSX
						PxD6JointDrive driveVal = HandleInfo->HandleData2.ConstraintData->getDrive(PxD6Drive::Enum::eTWIST);
						driveVal.flags &= ~PxD6JointDriveFlag::eACCELERATION;
						HandleInfo->HandleData2.ConstraintData->setDrive(PxD6Drive::Enum::eTWIST, driveVal);
#endif
					}
				}

				FPhysicsInterface::SetDrivePosition(HandleInfo->HandleData2, FVector::ZeroVector);
				FPhysicsInterface::SetDriveOrientation(HandleInfo->HandleData2, FQuat::Identity);
			}
			else
			{
				if (Grip->GripCollisionType == EGripCollisionType::InteractiveHybridCollisionWithPhysics)
				{
					// Do not effect damping, just increase stiffness so that it is stronger
					// Default multiplier
					Stiffness *= HYBRID_PHYSICS_GRIP_MULTIPLIER;
					AngularStiffness *= HYBRID_PHYSICS_GRIP_MULTIPLIER;
				}

				HandleInfo->LinConstraint.XDrive.Damping = Damping;
				HandleInfo->LinConstraint.XDrive.Stiffness = Stiffness;
				HandleInfo->LinConstraint.YDrive.Damping = Damping;
				HandleInfo->LinConstraint.YDrive.Stiffness = Stiffness;
				HandleInfo->LinConstraint.ZDrive.Damping = Damping;
				HandleInfo->LinConstraint.ZDrive.Stiffness = Stiffness;

				FPhysicsInterface::UpdateLinearDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->LinConstraint);

				HandleInfo->AngConstraint.TwistDrive.Damping = AngularDamping;
				HandleInfo->AngConstraint.TwistDrive.Stiffness = AngularStiffness;
				FPhysicsInterface::UpdateAngularDrive_AssumesLocked(HandleInfo->HandleData2, HandleInfo->AngConstraint);
			}

		}
		return true;
	}

	return false;
}

bool UGripMotionControllerComponent::GetPhysicsJointLength(const FBPActorGripInformation &GrippedActor, UPrimitiveComponent * rootComp, FVector & LocOut)
{
	if (!GrippedActor.GrippedObject)
		return false;

	FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(GrippedActor);

	if (!HandleInfo || !HandleInfo->KinActorData2.IsValid())
		return false;

	if (!HandleInfo->HandleData2.IsValid())
		return false;
	// This is supposed to be the difference between the actor and the kinactor / constraint base
	
	FTransform tran3 = FPhysicsInterface::GetLocalPose(HandleInfo->HandleData2, EConstraintFrame::Frame1);

	FTransform rr;
	FBodyInstance* rBodyInstance = rootComp->GetBodyInstance(GrippedActor.GrippedBoneName);
	if (!rBodyInstance || !rBodyInstance->IsValidBodyInstance())
	{
		rr = rootComp->GetComponentTransform();
		// Physx location throws out scale, this is where the problem was
		rr.SetScale3D(FVector(1, 1, 1));
	}
	else
		rr = rBodyInstance->GetUnrealWorldTransform();

	// Make the local pose global
	tran3 = tran3 * rr;

	// Get the global pose for the kin actor
	FTransform kinPose = FTransform::Identity;
	FPhysicsCommand::ExecuteRead(HandleInfo->KinActorData2, [&](const FPhysicsActorHandle & Actor)
	{
		kinPose = FPhysicsInterface::GetGlobalPose_AssumesLocked(Actor);
	});

	// Return the difference
	LocOut = FTransform::SubtractTranslations(kinPose, tran3);

	return true;
}

void UGripMotionControllerComponent::UpdatePhysicsHandleTransform(const FBPActorGripInformation &GrippedActor, const FTransform& NewTransform)
{
	if (!GrippedActor.GrippedObject)
		return;

	FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(GrippedActor);

	if (!HandleInfo || !HandleInfo->KinActorData2.IsValid())
		return;

	// Debug draw for COM movement with physics grips
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (GripMotionControllerCvars::DrawDebugGripCOM)
	{
		UPrimitiveComponent* me = Cast<UPrimitiveComponent>(GrippedActor.GripTargetType == EGripTargetType::ActorGrip ? GrippedActor.GetGrippedActor()->GetRootComponent() : GrippedActor.GetGrippedComponent());
		FVector curCOMPosition = me->GetBodyInstance(GrippedActor.GrippedBoneName)->GetCOMPosition();
		DrawDebugSphere(GetWorld(), curCOMPosition, 4, 32, FColor::Red, false);
		DrawDebugSphere(GetWorld(), (HandleInfo->COMPosition * (HandleInfo->RootBoneRotation * NewTransform)).GetLocation(), 4, 32, FColor::Cyan, false);
	}
#endif

	// Don't call moveKinematic if it hasn't changed - that will stop bodies from going to sleep.
	if (!HandleInfo->LastPhysicsTransform.EqualsNoScale(NewTransform))
	{
		HandleInfo->LastPhysicsTransform = NewTransform;
		HandleInfo->LastPhysicsTransform.SetScale3D(FVector(1.0f));
		FPhysicsCommand::ExecuteWrite(HandleInfo->KinActorData2, [&](const FPhysicsActorHandle & Actor)
		{
			FPhysicsInterface::SetKinematicTarget_AssumesLocked(Actor, HandleInfo->COMPosition * (HandleInfo->RootBoneRotation * HandleInfo->LastPhysicsTransform));
		});
	}
}

static void PullBackHitComp(FHitResult& Hit, const FVector& Start, const FVector& End, const float Dist)
{
	const float DesiredTimeBack = FMath::Clamp(0.1f, 0.1f / Dist, 1.f / Dist) + 0.001f;
	Hit.Time = FMath::Clamp(Hit.Time - DesiredTimeBack, 0.f, 1.f);
}

bool UGripMotionControllerComponent::CheckComponentWithSweep(UPrimitiveComponent * ComponentToCheck, FVector Move, FRotator newOrientation, bool bSkipSimulatingComponents/*,  bool &bHadBlockingHitOut*/)
{
	TArray<FHitResult> Hits;
	// WARNING: HitResult is only partially initialized in some paths. All data is valid only if bFilledHitResult is true.
	FHitResult BlockingHit(NoInit);
	BlockingHit.bBlockingHit = false;
	BlockingHit.Time = 1.f;
	bool bFilledHitResult = false;
	bool bMoved = false;
	bool bIncludesOverlapsAtEnd = false;
	bool bRotationOnly = false;

	UPrimitiveComponent *root = ComponentToCheck;

	if (!root || !root->IsQueryCollisionEnabled())
		return false;

	FVector start(root->GetComponentLocation());

	const bool bCollisionEnabled = root->IsQueryCollisionEnabled();

	if (bCollisionEnabled)
	{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (!root->IsRegistered())
		{
			UE_LOG(LogVRMotionController, Warning, TEXT("MovedComponent %s not initialized in grip motion controller"), *root->GetFullName());
		}
#endif

		UWorld* const MyWorld = GetWorld();
		FComponentQueryParams Params(TEXT("sweep_params"), root->GetOwner());

		FCollisionResponseParams ResponseParam;
		root->InitSweepCollisionParams(Params, ResponseParam);

		FVector end = start + Move;
		bool const bHadBlockingHit = MyWorld->ComponentSweepMulti(Hits, root, start, end, newOrientation.Quaternion(), Params);

		if (Hits.Num() > 0)
		{
			const float DeltaSize = FVector::Dist(start, end);
			for (int32 HitIdx = 0; HitIdx < Hits.Num(); HitIdx++)
			{
				PullBackHitComp(Hits[HitIdx], start, end, DeltaSize);
			}
		}

		if (bHadBlockingHit)
		{
			int32 BlockingHitIndex = INDEX_NONE;
			float BlockingHitNormalDotDelta = BIG_NUMBER;
			for (int32 HitIdx = 0; HitIdx < Hits.Num(); HitIdx++)
			{
				const FHitResult& TestHit = Hits[HitIdx];

				// Ignore the owning actor to the motion controller
				if (TestHit.Actor == this->GetOwner() || (bSkipSimulatingComponents && TestHit.Component->IsSimulatingPhysics()))
				{
					if (Hits.Num() == 1)
					{
						//bHadBlockingHitOut = false;
						return false;
					}
					else
						continue;
				}

				if (TestHit.bBlockingHit && TestHit.IsValidBlockingHit())
				{
					if (TestHit.Time == 0.f)
					{
						// We may have multiple initial hits, and want to choose the one with the normal most opposed to our movement.
						const float NormalDotDelta = (TestHit.ImpactNormal | Move);
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
					//}
				}
			}

			// Update blocking hit, if there was a valid one.
			if (BlockingHitIndex >= 0)
			{
				BlockingHit = Hits[BlockingHitIndex];
				bFilledHitResult = true;
			}
		}
	}

	// Handle blocking hit notifications. Avoid if pending kill (which could happen after overlaps).
	if (BlockingHit.bBlockingHit && !root->IsPendingKill())
	{
		check(bFilledHitResult);
		if (root->IsDeferringMovementUpdates())
		{
			FScopedMovementUpdate* ScopedUpdate = root->GetCurrentScopedMovement();
			ScopedUpdate->AppendBlockingHitAfterMove(BlockingHit);
		}
		else
		{
			
			if(root->GetOwner())
				root->DispatchBlockingHit(*root->GetOwner(), BlockingHit);
		}

		return true;
	}

	return false;
}

//=============================================================================
bool UGripMotionControllerComponent::GripPollControllerState(FVector& Position, FRotator& Orientation , float WorldToMetersScale)
{
	// Not calling PollControllerState from the parent because its private.......

	bool bIsInGameThread = IsInGameThread();

	if (bHasAuthority)
	{
		// New iteration and retrieval for 4.12
		TArray<IMotionController*> MotionControllers = IModularFeatures::Get().GetModularFeatureImplementations<IMotionController>(IMotionController::GetModularFeatureName());
		for (auto MotionController : MotionControllers)
		{
			if (MotionController == nullptr)
			{
				continue;
			}
			
			if(bIsInGameThread)
				CurrentTrackingStatus = MotionController->GetControllerTrackingStatus(PlayerIndex, MotionSource);

			if (MotionController->GetControllerOrientationAndPosition(PlayerIndex, MotionSource, Orientation, Position, WorldToMetersScale))
			{
				if (bOffsetByHMD)
				{
					if (bIsInGameThread)
					{
						if (GEngine->XRSystem.IsValid() && GEngine->XRSystem->IsHeadTrackingAllowed())
						{
							FQuat curRot;
							FVector curLoc;
							if (GEngine->XRSystem->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, curRot, curLoc))
							{
								curLoc.Z = 0;
								LastLocationForLateUpdate = curLoc;
							}
							else
							{
								 // Keep last location instead
							}
						}
					}

					// #TODO: This is technically unsafe, need to use a seperate value like the transforms for the render thread
					// If I ever delete the simple char then this setup can just go away anyway though
					// It has a data race condition right now though
					Position -= LastLocationForLateUpdate;
				}

				if (bOffsetByControllerProfile)
				{
					FTransform FinalControllerTransform(Orientation,Position);
					if (bIsInGameThread)
					{
						FinalControllerTransform = CurrentControllerProfileTransform * FinalControllerTransform;
					}
					else
					{
						FinalControllerTransform = GripRenderThreadProfileTransform * FinalControllerTransform;
					}
					
					Orientation = FinalControllerTransform.Rotator();
					Position = FinalControllerTransform.GetTranslation();
				}

				// Render thread also calls this, shouldn't be flagging this event in the render thread.
				if (bIsInGameThread)
				{
					InUseMotionController = MotionController;
					OnMotionControllerUpdated();
					InUseMotionController = nullptr;
				}
							
				return true;
			}
		}

		// #NOTE: This was adding in 4.20, I presume to allow for HMDs as tracking sources for mixed reality.
		// Skipping all of my special logic here for now
		if (MotionSource == FXRMotionControllerBase::HMDSourceId)
		{
			IXRTrackingSystem* TrackingSys = GEngine->XRSystem.Get();
			if (TrackingSys)
			{
				FQuat OrientationQuat = FQuat::Identity;
				if (TrackingSys->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, OrientationQuat, Position))
				{
					Orientation = OrientationQuat.Rotator();
					return true;
				}
			}
		}
	}
	return false;
}

//=============================================================================
UGripMotionControllerComponent::FGripViewExtension::FGripViewExtension(const FAutoRegister& AutoRegister, UGripMotionControllerComponent* InMotionControllerComponent)
	: FSceneViewExtensionBase(AutoRegister)
	, MotionControllerComponent(InMotionControllerComponent)
{}


void UGripMotionControllerComponent::FGripViewExtension::PreRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	FTransform OldTransform;
	FTransform NewTransform;

	{
		FScopeLock ScopeLock(&CritSect);

		if (!MotionControllerComponent)
			return;

		// Find a view that is associated with this player.
		float WorldToMetersScale = -1.0f;
		for (const FSceneView* SceneView : InViewFamily.Views)
		{
			if (SceneView && SceneView->PlayerIndex == MotionControllerComponent->PlayerIndex)
			{
				WorldToMetersScale = SceneView->WorldToMetersScale;
				break;
			}
		}

		// If there are no views associated with this player use view 0.
		if (WorldToMetersScale < 0.0f)
		{
			check(InViewFamily.Views.Num() > 0);
			WorldToMetersScale = InViewFamily.Views[0]->WorldToMetersScale;
		}

		// Poll state for the most recent controller transform
		FVector Position = FVector::ZeroVector;
		FRotator Orientation = FRotator::ZeroRotator;

		if (!MotionControllerComponent->GripPollControllerState(Position, Orientation, WorldToMetersScale))
		{
			return;
		}

		OldTransform = MotionControllerComponent->GripRenderThreadRelativeTransform;
		NewTransform = FTransform(Orientation, Position, MotionControllerComponent->GripRenderThreadComponentScale);
	} // Release lock on motion controller component

	  // Tell the late update manager to apply the offset to the scene components
	LateUpdate.Apply_RenderThread(InViewFamily.Scene, OldTransform, NewTransform);
}

void UGripMotionControllerComponent::FGripViewExtension::PostRenderViewFamily_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& InViewFamily)
{
	if (!MotionControllerComponent)
	{
		return;
	}
	LateUpdate.PostRender_RenderThread();
}

bool UGripMotionControllerComponent::FGripViewExtension::IsActiveThisFrame(class FViewport* InViewport) const
{
	check(IsInGameThread());

	static const auto CVarEnableMotionControllerLateUpdate = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.EnableMotionControllerLateUpdate"));
	return MotionControllerComponent && !MotionControllerComponent->bDisableLowLatencyUpdate && CVarEnableMotionControllerLateUpdate->GetValueOnGameThread();
}

void UGripMotionControllerComponent::GetAllGrips(TArray<FBPActorGripInformation> &GripArray)
{
	GripArray.Append(GrippedObjects);
	GripArray.Append(LocallyGrippedObjects);
}

void UGripMotionControllerComponent::GetGrippedObjects(TArray<UObject*> &GrippedObjectsArray)
{
	for (int i = 0; i < GrippedObjects.Num(); ++i)
	{
		if (GrippedObjects[i].GrippedObject)
			GrippedObjectsArray.Add(GrippedObjects[i].GrippedObject);
	}

	for (int i = 0; i < LocallyGrippedObjects.Num(); ++i)
	{
		if (LocallyGrippedObjects[i].GrippedObject)
			GrippedObjectsArray.Add(LocallyGrippedObjects[i].GrippedObject);
	}

}

void UGripMotionControllerComponent::GetGrippedActors(TArray<AActor*> &GrippedObjectsArray)
{
	for (int i = 0; i < GrippedObjects.Num(); ++i)
	{
		if(GrippedObjects[i].GetGrippedActor())
			GrippedObjectsArray.Add(GrippedObjects[i].GetGrippedActor());
	}

	for (int i = 0; i < LocallyGrippedObjects.Num(); ++i)
	{
		if (LocallyGrippedObjects[i].GetGrippedActor())
			GrippedObjectsArray.Add(LocallyGrippedObjects[i].GetGrippedActor());
	}

}

void UGripMotionControllerComponent::GetGrippedComponents(TArray<UPrimitiveComponent*> &GrippedComponentsArray)
{
	for (int i = 0; i < GrippedObjects.Num(); ++i)
	{
		if (GrippedObjects[i].GetGrippedComponent())
			GrippedComponentsArray.Add(GrippedObjects[i].GetGrippedComponent());
	}

	for (int i = 0; i < LocallyGrippedObjects.Num(); ++i)
	{
		if (LocallyGrippedObjects[i].GetGrippedComponent())
			GrippedComponentsArray.Add(LocallyGrippedObjects[i].GetGrippedComponent());
	}
}

// Locally gripped functions

bool UGripMotionControllerComponent::Client_NotifyInvalidLocalGrip_Validate(UObject * LocallyGrippedObject)
{
	return true;
}

void UGripMotionControllerComponent::Client_NotifyInvalidLocalGrip_Implementation(UObject * LocallyGrippedObject)
{
	FBPActorGripInformation FoundGrip;
	EBPVRResultSwitch Result;
	GetGripByObject(FoundGrip, LocallyGrippedObject, Result);

	if (Result == EBPVRResultSwitch::OnFailed)
		return;

	// Drop it, server told us that it was a bad grip
	DropObjectByInterface(FoundGrip.GrippedObject);
}

bool UGripMotionControllerComponent::Server_NotifyLocalGripAddedOrChanged_Validate(const FBPActorGripInformation & newGrip)
{
	return true;
}

void UGripMotionControllerComponent::Server_NotifyLocalGripAddedOrChanged_Implementation(const FBPActorGripInformation & newGrip)
{
	if (!newGrip.GrippedObject || newGrip.GripMovementReplicationSetting != EGripMovementReplicationSettings::ClientSide_Authoritive)
	{
		Client_NotifyInvalidLocalGrip(newGrip.GrippedObject);
		return;
	}

	if (!LocallyGrippedObjects.Contains(newGrip))
	{
		int32 NewIndex = LocallyGrippedObjects.Add(newGrip);
		HandleGripReplication(LocallyGrippedObjects[NewIndex]);
		// Initialize the differences, clients will do this themselves on the rep back, this sets up the cache
		//HandleGripReplication(LocallyGrippedObjects[LocallyGrippedObjects.Num() - 1]);
	}
	else
	{
		int32 IndexFound;
		if (LocallyGrippedObjects.Find(newGrip, IndexFound))
		{
			FBPActorGripInformation OriginalGrip = LocallyGrippedObjects[IndexFound];
			LocallyGrippedObjects[IndexFound].RepCopy(newGrip);
			HandleGripReplication(LocallyGrippedObjects[IndexFound], &OriginalGrip);
		}
	}

	// Server has to call this themselves
	//OnRep_LocallyGrippedObjects();
}


bool UGripMotionControllerComponent::Server_NotifyLocalGripRemoved_Validate(uint8 GripID, const FTransform_NetQuantize &TransformAtDrop, FVector_NetQuantize100 AngularVelocity, FVector_NetQuantize100 LinearVelocity)
{
	return true;
}

void UGripMotionControllerComponent::Server_NotifyLocalGripRemoved_Implementation(uint8 GripID, const FTransform_NetQuantize &TransformAtDrop, FVector_NetQuantize100 AngularVelocity, FVector_NetQuantize100 LinearVelocity)
{
	FBPActorGripInformation FoundGrip;
	EBPVRResultSwitch Result;
	GetGripByID(FoundGrip, GripID, Result);

	if (Result == EBPVRResultSwitch::OnFailed)
		return;

	switch (FoundGrip.GripTargetType)
	{
	case EGripTargetType::ActorGrip:
		FoundGrip.GetGrippedActor()->SetActorTransform(TransformAtDrop, false, nullptr, ETeleportType::TeleportPhysics); break;
	case EGripTargetType::ComponentGrip:
		FoundGrip.GetGrippedComponent()->SetWorldTransform(TransformAtDrop, false, nullptr, ETeleportType::TeleportPhysics); break;
	default:break;
	}

	if (!DropObjectByInterface(nullptr, FoundGrip.GripID, AngularVelocity, LinearVelocity))
	{
		DropGrip(FoundGrip, false, AngularVelocity, LinearVelocity);
	}
}


bool UGripMotionControllerComponent::Server_NotifySecondaryAttachmentChanged_Validate(
	uint8 GripID,
	const FBPSecondaryGripInfo& SecondaryGripInfo)
{
	return true;
}

void UGripMotionControllerComponent::Server_NotifySecondaryAttachmentChanged_Implementation(
	uint8 GripID,
	const FBPSecondaryGripInfo& SecondaryGripInfo)
{

	FBPActorGripInformation * GripInfo = LocallyGrippedObjects.FindByKey(GripID);
	if (GripInfo != nullptr)
	{
		FBPActorGripInformation OriginalGrip = *GripInfo;

		// I override the = operator now so that it won't set the lerp components
		GripInfo->SecondaryGripInfo.RepCopy(SecondaryGripInfo);

		// Initialize the differences, clients will do this themselves on the rep back
		HandleGripReplication(*GripInfo, &OriginalGrip);
	}

}

bool UGripMotionControllerComponent::Server_NotifySecondaryAttachmentChanged_Retain_Validate(
	uint8 GripID,
	const FBPSecondaryGripInfo& SecondaryGripInfo, const FTransform_NetQuantize & NewRelativeTransform)
{
	return true;
}

void UGripMotionControllerComponent::Server_NotifySecondaryAttachmentChanged_Retain_Implementation(
	uint8 GripID,
	const FBPSecondaryGripInfo& SecondaryGripInfo, const FTransform_NetQuantize & NewRelativeTransform)
{

	FBPActorGripInformation * GripInfo = LocallyGrippedObjects.FindByKey(GripID);
	if (GripInfo != nullptr)
	{
		FBPActorGripInformation OriginalGrip = *GripInfo;

		// I override the = operator now so that it won't set the lerp components
		GripInfo->SecondaryGripInfo.RepCopy(SecondaryGripInfo);
		GripInfo->RelativeTransform = NewRelativeTransform;

		// Initialize the differences, clients will do this themselves on the rep back
		HandleGripReplication(*GripInfo, &OriginalGrip);
	}

}
void UGripMotionControllerComponent::GetControllerDeviceID(FXRDeviceId & DeviceID, EBPVRResultSwitch &Result, bool bCheckOpenVROnly)
{
	EControllerHand ControllerHandIndex;
	if (!FXRMotionControllerBase::GetHandEnumForSourceName(MotionSource, ControllerHandIndex))
	{
		Result = EBPVRResultSwitch::OnFailed;
		return;
	}

	TArray<IXRSystemAssets*> XRAssetSystems = IModularFeatures::Get().GetModularFeatureImplementations<IXRSystemAssets>(IXRSystemAssets::GetModularFeatureName());
	for (IXRSystemAssets* AssetSys : XRAssetSystems)
	{
		if (bCheckOpenVROnly && !AssetSys->GetSystemName().IsEqual(FName(TEXT("SteamVR"))))
			continue;

		const int32 XRID = AssetSys->GetDeviceId(ControllerHandIndex);

		if (XRID != INDEX_NONE)
		{
			DeviceID = FXRDeviceId(AssetSys, XRID);
			Result = EBPVRResultSwitch::OnSucceeded;
			return;
		}
	}

	DeviceID = FXRDeviceId();
	Result = EBPVRResultSwitch::OnFailed;
	return;
}


/*
*
*	Custom late update manager implementation
*
*/

FExpandedLateUpdateManager::FExpandedLateUpdateManager()
	: LateUpdateGameWriteIndex(0)
	, LateUpdateRenderReadIndex(0)
{
	SkipLateUpdate[0] = false;
	SkipLateUpdate[1] = false;
}

void FExpandedLateUpdateManager::Setup(const FTransform& ParentToWorld, UGripMotionControllerComponent* Component, bool bSkipLateUpdate)
{
	if (!Component)
		return;

	check(IsInGameThread());

	LateUpdateParentToWorld[LateUpdateGameWriteIndex] = ParentToWorld;
	LateUpdatePrimitives[LateUpdateGameWriteIndex].Reset();
	SkipLateUpdate[LateUpdateGameWriteIndex] = bSkipLateUpdate;

	TArray<USceneComponent*> ComponentsThatSkipLateUpdate;

	//Add additional late updates registered to this controller that aren't children and aren't gripped
	//This array is editable in blueprint and can be used for things like arms or the like.
	for (UPrimitiveComponent* primComp : Component->AdditionalLateUpdateComponents)
	{
		if (primComp)
			GatherLateUpdatePrimitives(primComp);
	}


	ProcessGripArrayLateUpdatePrimitives(Component, Component->LocallyGrippedObjects, ComponentsThatSkipLateUpdate);
	ProcessGripArrayLateUpdatePrimitives(Component, Component->GrippedObjects, ComponentsThatSkipLateUpdate);

	GatherLateUpdatePrimitives(Component, &ComponentsThatSkipLateUpdate);

	LateUpdateGameWriteIndex = (LateUpdateGameWriteIndex + 1) % 2;
}

bool FExpandedLateUpdateManager::GetSkipLateUpdate_RenderThread() const
{
	return SkipLateUpdate[LateUpdateRenderReadIndex];
}


void FExpandedLateUpdateManager::Apply_RenderThread(FSceneInterface* Scene, const FTransform& OldRelativeTransform, const FTransform& NewRelativeTransform)
{
	check(IsInRenderingThread());

	if (!LateUpdatePrimitives[LateUpdateRenderReadIndex].Num())
	{
		return;
	}

	if (GetSkipLateUpdate_RenderThread())
	{
		return;
	}

	const FTransform OldCameraTransform = OldRelativeTransform * LateUpdateParentToWorld[LateUpdateRenderReadIndex];
	const FTransform NewCameraTransform = NewRelativeTransform * LateUpdateParentToWorld[LateUpdateRenderReadIndex];
	const FMatrix LateUpdateTransform = (OldCameraTransform.Inverse() * NewCameraTransform).ToMatrixWithScale();

	bool bIndicesHaveChanged = false;

	// Apply delta to the cached scene proxies
	// Also check whether any primitive indices have changed, in case the scene has been modified in the meantime.
	for (auto PrimitivePair : LateUpdatePrimitives[LateUpdateRenderReadIndex])
	{
		FPrimitiveSceneInfo* RetrievedSceneInfo = Scene->GetPrimitiveSceneInfo(PrimitivePair.Value);
		FPrimitiveSceneInfo* CachedSceneInfo = PrimitivePair.Key;

		// If the retrieved scene info is different than our cached scene info then the scene has changed in the meantime
		// and we need to search through the entire scene to make sure it still exists.
		if (CachedSceneInfo != RetrievedSceneInfo)
		{
			bIndicesHaveChanged = true;
			break; // No need to continue here, as we are going to brute force the scene primitives below anyway.
		}
		else if (CachedSceneInfo->Proxy)
		{
			CachedSceneInfo->Proxy->ApplyLateUpdateTransform(LateUpdateTransform);
			PrimitivePair.Value = -1; // Set the cached index to -1 to indicate that this primitive was already processed
		}
	}

	// Indices have changed, so we need to scan the entire scene for primitives that might still exist
	if (bIndicesHaveChanged)
	{
		int32 Index = 0;
		FPrimitiveSceneInfo* RetrievedSceneInfo = Scene->GetPrimitiveSceneInfo(Index++);
		while (RetrievedSceneInfo)
		{
			if (RetrievedSceneInfo->Proxy && LateUpdatePrimitives[LateUpdateRenderReadIndex].Contains(RetrievedSceneInfo) && LateUpdatePrimitives[LateUpdateRenderReadIndex][RetrievedSceneInfo] >= 0)
			{
				RetrievedSceneInfo->Proxy->ApplyLateUpdateTransform(LateUpdateTransform);
			}
			RetrievedSceneInfo = Scene->GetPrimitiveSceneInfo(Index++);
		}
	}
}

void FExpandedLateUpdateManager::PostRender_RenderThread()
{
	LateUpdatePrimitives[LateUpdateRenderReadIndex].Reset();
	SkipLateUpdate[LateUpdateRenderReadIndex] = false;
	LateUpdateRenderReadIndex = (LateUpdateRenderReadIndex + 1) % 2;
}

void FExpandedLateUpdateManager::CacheSceneInfo(USceneComponent* Component)
{
	// If a scene proxy is present, cache it
	UPrimitiveComponent* PrimitiveComponent = dynamic_cast<UPrimitiveComponent*>(Component);
	if (PrimitiveComponent && PrimitiveComponent->SceneProxy)
	{
		FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveComponent->SceneProxy->GetPrimitiveSceneInfo();
		if (PrimitiveSceneInfo)
		{
			LateUpdatePrimitives[LateUpdateGameWriteIndex].Emplace(PrimitiveSceneInfo, PrimitiveSceneInfo->GetIndex());
		}
	}
}

void FExpandedLateUpdateManager::GatherLateUpdatePrimitives(USceneComponent* ParentComponent, TArray<USceneComponent*> *SkipComponentList)
{
	CacheSceneInfo(ParentComponent);
	TArray<USceneComponent*> DirectComponents;

	if (SkipComponentList && SkipComponentList->Num())
	{
		// Skip attachment grips, slower logic
		ParentComponent->GetChildrenComponents(false, DirectComponents);

		TArray<USceneComponent*> SubComponents;
		for (USceneComponent* Component : DirectComponents)
		{
			if (Component != nullptr && (SkipComponentList ? !SkipComponentList->Contains(Component) : true))
			{
				CacheSceneInfo(Component);
				Component->GetChildrenComponents(true, SubComponents);

				for (USceneComponent* SubComponent : SubComponents)
				{
					if (Component != nullptr)
					{
						CacheSceneInfo(SubComponent);
					}
				}
			}

		}
	}
	else
	{
		// Std late updates
		ParentComponent->GetChildrenComponents(true, DirectComponents);
		for (USceneComponent* Component : DirectComponents)
		{
			if (Component != nullptr)
			{
				CacheSceneInfo(Component);
			}
		}
	}
}

void FExpandedLateUpdateManager::ProcessGripArrayLateUpdatePrimitives(UGripMotionControllerComponent * MotionControllerComponent, TArray<FBPActorGripInformation> & GripArray, TArray<USceneComponent*> &SkipComponentList)
{
	for (FBPActorGripInformation actor : GripArray)
	{
		// Skip actors that are colliding if turning off late updates during collision.
		// Also skip turning off late updates for SweepWithPhysics, as it should always be locked to the hand
		if (!actor.GrippedObject || actor.GripCollisionType == EGripCollisionType::EventsOnly)
			continue;

		// Handle late updates even with attachment, we need to add it to a skip list for the primary gatherer to process
		if (actor.GripCollisionType == EGripCollisionType::AttachmentGrip)
		{
			switch (actor.GripTargetType)
			{
			case EGripTargetType::ActorGrip:
			{
				if (AActor * GrippedActor = actor.GetGrippedActor())
				{
					SkipComponentList.Add(GrippedActor->GetRootComponent());
				}
			}break;
			case EGripTargetType::ComponentGrip:
			{
				if (UPrimitiveComponent* GrippedComponent = actor.GetGrippedComponent())
				{
					SkipComponentList.Add(GrippedComponent);
				}
			}break;
			}
			//continue;
		}

		// Don't allow late updates with server sided movement, there is no point
		if (actor.GripMovementReplicationSetting == EGripMovementReplicationSettings::ForceServerSideMovement && !MotionControllerComponent->IsServer())
			continue;

		// Don't late update paused grips
		if (actor.bIsPaused)
			continue;

		switch (actor.GripLateUpdateSetting)
		{
		case EGripLateUpdateSettings::LateUpdatesAlwaysOff:
		{
			continue;
		}break;
		case EGripLateUpdateSettings::NotWhenColliding:
		{
			if (actor.bColliding && actor.GripCollisionType != EGripCollisionType::SweepWithPhysics && 
				actor.GripCollisionType != EGripCollisionType::PhysicsOnly)
				continue;
		}break;
		case EGripLateUpdateSettings::NotWhenDoubleGripping:
		{
			if (actor.SecondaryGripInfo.bHasSecondaryAttachment)
				continue;
		}break;
		case EGripLateUpdateSettings::NotWhenCollidingOrDoubleGripping:
		{
			if (
				(actor.bColliding && actor.GripCollisionType != EGripCollisionType::SweepWithPhysics && actor.GripCollisionType != EGripCollisionType::PhysicsOnly) ||
				(actor.SecondaryGripInfo.bHasSecondaryAttachment)
				)
			{
				continue;
			}
		}break;
		case EGripLateUpdateSettings::LateUpdatesAlwaysOn:
		default:
		{}break;
		}

		// Don't run late updates if we have a grip script that denies it
		if (actor.GrippedObject->GetClass()->ImplementsInterface(UVRGripInterface::StaticClass()))
		{
			TArray<UVRGripScriptBase*> GripScripts;
			if (IVRGripInterface::Execute_GetGripScripts(actor.GrippedObject, GripScripts))
			{
				bool bContinueOn = false;
				for (UVRGripScriptBase* Script : GripScripts)
				{
					if (Script && Script->IsScriptActive() && Script->Wants_DenyLateUpdates())
					{
						bContinueOn = true;
						break;
					}
				}

				if (bContinueOn)
					continue;
			}
		}

		// Get late update primitives
		switch (actor.GripTargetType)
		{
		case EGripTargetType::ActorGrip:
			//case EGripTargetType::InteractibleActorGrip:
		{
			AActor * pActor = actor.GetGrippedActor();
			if (pActor)
			{
				if (USceneComponent * rootComponent = pActor->GetRootComponent())
				{
					GatherLateUpdatePrimitives(rootComponent);
				}
			}

		}break;

		case EGripTargetType::ComponentGrip:
			//case EGripTargetType::InteractibleComponentGrip:
		{
			UPrimitiveComponent * cPrimComp = actor.GetGrippedComponent();
			if (cPrimComp)
			{
				GatherLateUpdatePrimitives(cPrimComp);
			}
		}break;
		}
	}
}

void UGripMotionControllerComponent::GetHandType(EControllerHand& Hand)
{
	if (!FXRMotionControllerBase::GetHandEnumForSourceName(MotionSource, Hand))
	{
		Hand = EControllerHand::Left;
	}
}

void UGripMotionControllerComponent::SetCustomPivotComponent(USceneComponent * NewCustomPivotComponent)
{
	CustomPivotComponent = NewCustomPivotComponent;
}

FTransform UGripMotionControllerComponent::ConvertToControllerRelativeTransform(const FTransform & InTransform)
{
	return InTransform.GetRelativeTransform(this->GetComponentTransform());
}

FTransform UGripMotionControllerComponent::ConvertToGripRelativeTransform(const FTransform& GrippedActorTransform, const FTransform & InTransform)
{
	return InTransform.GetRelativeTransform(GrippedActorTransform);
}

bool UGripMotionControllerComponent::GetIsObjectHeld(const UObject * ObjectToCheck)
{
	if (!ObjectToCheck)
		return false;

	return (GrippedObjects.FindByKey(ObjectToCheck) || LocallyGrippedObjects.FindByKey(ObjectToCheck));
}

bool UGripMotionControllerComponent::GetIsHeld(const AActor * ActorToCheck)
{
	if (!ActorToCheck)
		return false;

	return (GrippedObjects.FindByKey(ActorToCheck) || LocallyGrippedObjects.FindByKey(ActorToCheck));
}

bool UGripMotionControllerComponent::GetIsComponentHeld(const UPrimitiveComponent * ComponentToCheck)
{
	if (!ComponentToCheck)
		return false;

	return (GrippedObjects.FindByKey(ComponentToCheck) || LocallyGrippedObjects.FindByKey(ComponentToCheck));

	return false;
}

bool UGripMotionControllerComponent::GetIsSecondaryAttachment(const USceneComponent * ComponentToCheck, FBPActorGripInformation & Grip)
{
	if (!ComponentToCheck)
		return false;

	for (int i = 0; i < GrippedObjects.Num(); ++i)
	{
		if (GrippedObjects[i].SecondaryGripInfo.bHasSecondaryAttachment && GrippedObjects[i].SecondaryGripInfo.SecondaryAttachment == ComponentToCheck)
		{
			Grip = GrippedObjects[i];
			return true;
		}
	}

	for (int i = 0; i < LocallyGrippedObjects.Num(); ++i)
	{
		if (LocallyGrippedObjects[i].SecondaryGripInfo.bHasSecondaryAttachment && LocallyGrippedObjects[i].SecondaryGripInfo.SecondaryAttachment == ComponentToCheck)
		{
			Grip = LocallyGrippedObjects[i];
			return true;
		}
	}

	return false;
}

bool UGripMotionControllerComponent::HasGrippedObjects()
{
	return GrippedObjects.Num() > 0 || LocallyGrippedObjects.Num() > 0;
}

bool UGripMotionControllerComponent::SetUpPhysicsHandle_BP(const FBPActorGripInformation &Grip)
{
	return SetUpPhysicsHandle(Grip);
}

bool UGripMotionControllerComponent::DestroyPhysicsHandle_BP(const FBPActorGripInformation &Grip)
{
	return DestroyPhysicsHandle(Grip);
}

bool UGripMotionControllerComponent::UpdatePhysicsHandle_BP(const FBPActorGripInformation& Grip, bool bFullyRecreate)
{
	return UpdatePhysicsHandle(Grip.GripID, bFullyRecreate);
}

bool UGripMotionControllerComponent::GetPhysicsHandleSettings(const FBPActorGripInformation& Grip, FBPAdvancedPhysicsHandleSettings& PhysicsHandleSettingsOut)
{
	FBPActorPhysicsHandleInformation * HandleInfo = GetPhysicsGrip(Grip);
	
	if (!HandleInfo)
		return false;

	PhysicsHandleSettingsOut.FillFrom(HandleInfo);
	return true;
}

bool UGripMotionControllerComponent::SetPhysicsHandleSettings(const FBPActorGripInformation& Grip, const FBPAdvancedPhysicsHandleSettings& PhysicsHandleSettingsIn)
{
	FBPActorPhysicsHandleInformation* HandleInfo = GetPhysicsGrip(Grip);

	if (!HandleInfo)
		return false;

	PhysicsHandleSettingsIn.FillTo(HandleInfo);
	return UpdatePhysicsHandle(Grip, true);
}


void UGripMotionControllerComponent::UpdatePhysicsHandleTransform_BP(const FBPActorGripInformation &GrippedActor, const FTransform& NewTransform)
{
	return UpdatePhysicsHandleTransform(GrippedActor, NewTransform);
}

bool UGripMotionControllerComponent::GetGripDistance_BP(FBPActorGripInformation &Grip, FVector ExpectedLocation, float & CurrentDistance)
{
	if (!Grip.GrippedObject)
		return false;

	UPrimitiveComponent * RootComp = nullptr;

	if (Grip.GripTargetType == EGripTargetType::ActorGrip)
	{
		RootComp = Cast<UPrimitiveComponent>(Grip.GetGrippedActor()->GetRootComponent());
	}
	else
		RootComp = Grip.GetGrippedComponent();

	if (!RootComp)
		return false;

	FVector CheckDistance;
	if (!GetPhysicsJointLength(Grip, RootComp, CheckDistance))
	{
		CheckDistance = (ExpectedLocation - RootComp->GetComponentLocation());
	}

	// Set grip distance now for people to use
	CurrentDistance = CheckDistance.Size();
	return true;
}

bool UGripMotionControllerComponent::GripControllerIsTracked() const
{
	return bTracked;
}