// HoloLensInputConverter.h
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "HoloLensInputConverter.generated.h"

UCLASS( ClassGroup=(Custom), meta=(BlueprintSpawnableComponent) )
class FINAL_PROJECT_API UHoloLensInputConverter : public UActorComponent
{
	GENERATED_BODY()

public:	
	UHoloLensInputConverter();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// Convert mouse input to touch gestures for HoloLens 2
	UFUNCTION(BlueprintCallable, Category = "HoloLens Input")
	void EnableHoloLensInputConversion(bool bEnable = true);

	// Check if we're running on HoloLens 2
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "HoloLens Input")
	bool IsHoloLens2Platform() const;

	// Simulate touch input from mouse
	UFUNCTION(BlueprintCallable, Category = "HoloLens Input")
	void SimulateTouchFromMouse(FVector2D MousePosition, bool bPressed);

	// Handle air tap gestures (left click equivalent)
	UFUNCTION(BlueprintCallable, Category = "HoloLens Input")
	void HandleAirTapGesture(FVector2D GazePosition);

	// Handle tap and hold gestures (right click equivalent)
	UFUNCTION(BlueprintCallable, Category = "HoloLens Input")
	void HandleTapAndHoldGesture(FVector2D GazePosition, float HoldDuration);

protected:
	// Input event handling
	bool OnMouseButtonDown(const FPointerEvent& MouseEvent);
	bool OnMouseButtonUp(const FPointerEvent& MouseEvent);
	bool OnMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent);

	// Touch simulation
	void SimulateTouchStart(FVector2D Position, int32 TouchIndex = 0);
	void SimulateTouchEnd(FVector2D Position, int32 TouchIndex = 0);
	void SimulateTouchMove(FVector2D Position, int32 TouchIndex = 0);

private:
	// Configuration
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings", meta = (AllowPrivateAccess = "true"))
	bool bInputConversionEnabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings", meta = (AllowPrivateAccess = "true"))
	float TapAndHoldThreshold;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HoloLens Settings", meta = (AllowPrivateAccess = "true"))
	float GazeRayDistance;

	// State tracking
	bool bIsLeftMouseDown;
	bool bIsRightMouseDown;
	FVector2D LastMousePosition;
	float RightClickStartTime;
	
	// Touch simulation state
	TMap<int32, bool> ActiveTouches;
	
	// Input event delegates
	FDelegateHandle MouseDownHandle;
	FDelegateHandle MouseUpHandle;
	FDelegateHandle MouseMoveHandle;
};