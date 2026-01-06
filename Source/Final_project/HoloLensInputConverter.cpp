// HoloLensInputConverter.cpp
#include "HoloLensInputConverter.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Input/TouchInterface.h"
#include "Misc/DateTime.h"

UHoloLensInputConverter::UHoloLensInputConverter()
{
	PrimaryComponentTick.bCanEverTick = true;
	
	// Default settings
	bInputConversionEnabled = true;
	TapAndHoldThreshold = 1.0f; // 1 second for tap and hold
	GazeRayDistance = 10000.0f; // 100 meters
	
	// Initialize state
	bIsLeftMouseDown = false;
	bIsRightMouseDown = false;
	RightClickStartTime = 0.0f;
	LastMousePosition = FVector2D::ZeroVector;
}

void UHoloLensInputConverter::BeginPlay()
{
	Super::BeginPlay();
	
	UE_LOG(LogTemp, Warning, TEXT("HoloLensInputConverter: Component started"));
	
	// Enable input conversion if we're on HoloLens 2 or in development
	if (IsHoloLens2Platform() || bInputConversionEnabled)
	{
		EnableHoloLensInputConversion(true);
		UE_LOG(LogTemp, Warning, TEXT("HoloLensInputConverter: Input conversion enabled"));
	}
}

void UHoloLensInputConverter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	EnableHoloLensInputConversion(false);
	Super::EndPlay(EndPlayReason);
}

void UHoloLensInputConverter::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	
	// Check for tap and hold completion
	if (bIsRightMouseDown && RightClickStartTime > 0.0f)
	{
		float CurrentTime = GetWorld()->GetTimeSeconds();
		float HoldDuration = CurrentTime - RightClickStartTime;
		
		if (HoldDuration >= TapAndHoldThreshold)
		{
			UE_LOG(LogTemp, Log, TEXT("HoloLensInput: Tap and hold gesture completed (%.2fs)"), HoldDuration);
			HandleTapAndHoldGesture(LastMousePosition, HoldDuration);
			RightClickStartTime = 0.0f; // Reset to prevent multiple triggers
		}
	}
}

void UHoloLensInputConverter::EnableHoloLensInputConversion(bool bEnable)
{
	if (!bEnable)
	{
		// Remove input delegates
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& SlateApp = FSlateApplication::Get();
			SlateApp.GetPlatformApplication()->Cursor->RemoveDelegate(MouseDownHandle);
			SlateApp.GetPlatformApplication()->Cursor->RemoveDelegate(MouseUpHandle);
			SlateApp.GetPlatformApplication()->Cursor->RemoveDelegate(MouseMoveHandle);
		}
		UE_LOG(LogTemp, Warning, TEXT("HoloLensInputConverter: Input conversion disabled"));
		return;
	}
	
	// Set up input event handling for mouse to touch conversion
	if (FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Warning, TEXT("HoloLensInputConverter: Setting up input event handlers"));
		
		// Note: In a real implementation, you would bind to the appropriate input events
		// This is a simplified approach for demonstration
		bInputConversionEnabled = bEnable;
	}
}

bool UHoloLensInputConverter::IsHoloLens2Platform() const
{
	// Check if we're running on HoloLens 2
#if PLATFORM_HOLOLENS
	return true;
#else
	// For development, you can simulate HoloLens mode
	return false;
#endif
}

void UHoloLensInputConverter::SimulateTouchFromMouse(FVector2D MousePosition, bool bPressed)
{
	if (!bInputConversionEnabled)
		return;
		
	UE_LOG(LogTemp, Log, TEXT("HoloLensInput: Converting mouse to touch at (%.1f, %.1f), Pressed: %s"), 
		MousePosition.X, MousePosition.Y, bPressed ? TEXT("True") : TEXT("False"));
	
	if (bPressed)
	{
		SimulateTouchStart(MousePosition);
	}
	else
	{
		SimulateTouchEnd(MousePosition);
	}
}

void UHoloLensInputConverter::HandleAirTapGesture(FVector2D GazePosition)
{
	UE_LOG(LogTemp, Warning, TEXT("HoloLensInput: Air tap gesture detected at (%.1f, %.1f)"), 
		GazePosition.X, GazePosition.Y);
	
	// Simulate a quick touch tap
	SimulateTouchStart(GazePosition);
	
	// In a real implementation, you might want to delay this slightly
	// For now, we'll simulate an immediate release
	SimulateTouchEnd(GazePosition);
	
	// Get the player controller to perform raycast for building selection
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FHitResult HitResult;
			FVector WorldLocation, WorldDirection;
			
			// Convert screen position to world ray
			if (PC->DeprojectScreenPositionToWorld(GazePosition.X, GazePosition.Y, WorldLocation, WorldDirection))
			{
				FVector StartLocation = WorldLocation;
				FVector EndLocation = StartLocation + (WorldDirection * GazeRayDistance);
				
				// Perform line trace
				if (World->LineTraceSingleByChannel(HitResult, StartLocation, EndLocation, ECC_Visibility))
				{
					UE_LOG(LogTemp, Warning, TEXT("HoloLensInput: Hit actor: %s"), 
						HitResult.GetActor() ? *HitResult.GetActor()->GetName() : TEXT("None"));
					
					// Here you could trigger building selection logic
					// For example, call a function on your BuildingEnergyDisplay actor
				}
			}
		}
	}
}

void UHoloLensInputConverter::HandleTapAndHoldGesture(FVector2D GazePosition, float HoldDuration)
{
	UE_LOG(LogTemp, Warning, TEXT("HoloLensInput: Tap and hold gesture completed at (%.1f, %.1f), Duration: %.2fs"), 
		GazePosition.X, GazePosition.Y, HoldDuration);
	
	// This is equivalent to a right-click context menu
	// You can trigger context-sensitive actions here
	
	// Example: Show building attributes widget
	if (UWorld* World = GetWorld())
	{
		// Find BuildingEnergyDisplay actor and show attributes
		for (TActorIterator<AActor> ActorItr(World); ActorItr; ++ActorItr)
		{
			AActor* Actor = *ActorItr;
			if (Actor && Actor->GetClass()->GetName().Contains(TEXT("BuildingEnergyDisplay")))
			{
				UE_LOG(LogTemp, Warning, TEXT("HoloLensInput: Found BuildingEnergyDisplay, triggering context action"));
				// Here you could call a function to show building details
				break;
			}
		}
	}
}

bool UHoloLensInputConverter::OnMouseButtonDown(const FPointerEvent& MouseEvent)
{
	if (!bInputConversionEnabled)
		return false;
	
	FVector2D MousePosition = MouseEvent.GetScreenSpacePosition();
	LastMousePosition = MousePosition;
	
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bIsLeftMouseDown = true;
		HandleAirTapGesture(MousePosition);
		return true;
	}
	else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		bIsRightMouseDown = true;
		RightClickStartTime = GetWorld()->GetTimeSeconds();
		UE_LOG(LogTemp, Log, TEXT("HoloLensInput: Right mouse down - starting tap and hold timer"));
		return true;
	}
	
	return false;
}

bool UHoloLensInputConverter::OnMouseButtonUp(const FPointerEvent& MouseEvent)
{
	if (!bInputConversionEnabled)
		return false;
	
	FVector2D MousePosition = MouseEvent.GetScreenSpacePosition();
	
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bIsLeftMouseDown = false;
		return true;
	}
	else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		bIsRightMouseDown = false;
		
		// Check if this was a quick release (not a full tap and hold)
		if (RightClickStartTime > 0.0f)
		{
			float CurrentTime = GetWorld()->GetTimeSeconds();
			float HoldDuration = CurrentTime - RightClickStartTime;
			
			if (HoldDuration < TapAndHoldThreshold)
			{
				UE_LOG(LogTemp, Log, TEXT("HoloLensInput: Quick right click (%.2fs) - treating as short tap"), HoldDuration);
				// Could trigger a different action for short taps
			}
		}
		
		RightClickStartTime = 0.0f;
		return true;
	}
	
	return false;
}

bool UHoloLensInputConverter::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (!bInputConversionEnabled)
		return false;
	
	LastMousePosition = MouseEvent.GetScreenSpacePosition();
	
	// If we're tracking a touch, update its position
	if (bIsLeftMouseDown || bIsRightMouseDown)
	{
		SimulateTouchMove(LastMousePosition);
	}
	
	return false;
}

void UHoloLensInputConverter::SimulateTouchStart(FVector2D Position, int32 TouchIndex)
{
	ActiveTouches.Add(TouchIndex, true);
	UE_LOG(LogTemp, Verbose, TEXT("HoloLensInput: Simulated touch start at (%.1f, %.1f), Index: %d"), 
		Position.X, Position.Y, TouchIndex);
	
	// In a real implementation, you would inject touch events into the input system
	// This would involve creating FTouchInputEvent and passing it to the appropriate handlers
}

void UHoloLensInputConverter::SimulateTouchEnd(FVector2D Position, int32 TouchIndex)
{
	ActiveTouches.Remove(TouchIndex);
	UE_LOG(LogTemp, Verbose, TEXT("HoloLensInput: Simulated touch end at (%.1f, %.1f), Index: %d"), 
		Position.X, Position.Y, TouchIndex);
}

void UHoloLensInputConverter::SimulateTouchMove(FVector2D Position, int32 TouchIndex)
{
	if (ActiveTouches.Contains(TouchIndex))
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("HoloLensInput: Simulated touch move to (%.1f, %.1f), Index: %d"), 
			Position.X, Position.Y, TouchIndex);
	}
}