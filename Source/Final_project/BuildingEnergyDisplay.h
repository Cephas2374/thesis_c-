// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Http.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "BuildingEnergyDisplay.generated.h"

// Forward declarations for UMG widgets
class UUserWidget;
class UTextBlock;

USTRUCT(BlueprintType)
struct FBuildingBoundingBox
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category = "Bounding Box")
	FVector MinBounds;
	
	UPROPERTY(BlueprintReadWrite, Category = "Bounding Box")
	FVector MaxBounds;
	
	UPROPERTY(BlueprintReadWrite, Category = "Bounding Box")
	FVector Center;
	
	UPROPERTY(BlueprintReadWrite, Category = "Bounding Box")
	FVector Size;
	
	FBuildingBoundingBox()
	{
		MinBounds = FVector::ZeroVector;
		MaxBounds = FVector::ZeroVector;
		Center = FVector::ZeroVector;
		Size = FVector::ZeroVector;
	}
};

UCLASS(Blueprintable)
class FINAL_PROJECT_API ABuildingEnergyDisplay : public AActor
{
	GENERATED_BODY()
	
public:
	ABuildingEnergyDisplay();

protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(BlueprintReadWrite, Category = "Building Energy")
	FString AccessToken;

	UPROPERTY(BlueprintReadWrite, Category = "Building Energy")
	FString ModifiedGmlId;

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void PreloadAllBuildingData(const FString& Token);

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void DisplayBuildingData(const FString& GmlId);

	void FetchBuildingEnergyData(const FString& GmlId, const FString& Token);

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void ClearCache();

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void RefreshBuildingCache();

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void AuthenticateAndLoadData();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void RefreshAccessToken(); // Use refresh token to get new access token
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void ApplyColorsToCSiumTileset();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	UMaterialInstanceDynamic* CreateBuildingEnergyMaterial();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Building Energy")
	bool IsDataLoaded() const { return bDataLoaded; }

	UFUNCTION(BlueprintCallable, Category = "Building Energy")
	void DisplayBuildingEnergyData(const FString& BuildingGmlId);
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Energy", meta = (DisplayName = "Building Energy Material"))
	UMaterialInstanceDynamic* BuildingEnergyMaterial;
	
	// UMG Widget for Building Information Display
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building Info Widget")
	TSubclassOf<UUserWidget> BuildingInfoWidgetClass;
	
	UPROPERTY(BlueprintReadOnly, Category = "Building Info Widget")
	UUserWidget* CurrentBuildingInfoWidget;
	
	// Single Building Display Control
	UPROPERTY(BlueprintReadOnly, Category = "Building Energy")
	FString CurrentlyDisplayedBuildingId;
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy Display")
	void AssignMaterialToCesiumTileset();
	
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Building Energy Display")
	void CreateMaterialForEditor();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy Display")
	void ApplyPerBuildingColorsToCesium();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy Display")
	void ApplyColorsUsingCesiumStyling();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy Display")
	void ApplyConditionalStylingToTileset();
	
	UFUNCTION(BlueprintCallable, Category = "Building Energy Display")
	void ApplyOfficialCesiumMetadataVisualization();
	
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Building Energy Display")
	void CreateTextureBasedMaterial();
	
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Building Energy Display")
	void CreatePerBuildingColorMaterial();
	
	void GetBuildingAttributes(const FString& BuildingKey, const FString& CommunityId, const FString& Token);

	void UpdateBuildingAttributes(const FString& BuildingKey, const FString& CommunityId, const FString& AttributesJson, const FString& Token);
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time Data")
	void FetchRealTimeEnergyData();
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time Data")  
	void ForceRealTimeRefresh();
	
	// WebSocket Real-Time Energy Functions
	UFUNCTION(BlueprintCallable, Category = "WebSocket Energy")
	void ConnectEnergyWebSocket();
	
	UFUNCTION(BlueprintCallable, Category = "WebSocket Energy")
	void DisconnectEnergyWebSocket();
	
	void OnEnergyWebSocketConnected();
	void OnEnergyWebSocketConnectionError(const FString& Error);
	void OnEnergyWebSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnEnergyWebSocketMessage(const FString& Message);
	void ProcessEnergyWebSocketUpdate(const FString& JsonData);
	
	// Coordinate-Based Building Validation Functions
	UFUNCTION(BlueprintCallable, Category = "Coordinate Validation")
	bool ValidateBuildingPosition(const FVector& ClickPosition, const FString& GmlId);
	
	FBuildingBoundingBox CreateBuildingBoundingBox(const FString& GmlId);
	bool IsPointInBoundingBox(const FVector& Point, const FBuildingBoundingBox& BoundingBox);
	bool IsPointInBuildingBounds(const FVector& Point, const FString& BuildingCoordinates);
	bool ParseBuildingCoordinates(const FString& CoordinatesString, TArray<FVector>& OutCoordinates);
	bool IsPointInPolygon(const FVector& Point, const TArray<FVector>& PolygonVertices);
	FString GetBuildingByCoordinates(const FVector& ClickPosition);
	void StoreBuildingCoordinates(const FString& GmlId, const FString& CoordinatesData);
	
	void TestBuildingAttributesAPI();

	UFUNCTION(BlueprintCallable, Category = "Building Attributes")
	void ShowBuildingAttributesForm(const FString& BuildingGmlId);
	
	// UMG Widget Functions for Building Information Display
	UFUNCTION(BlueprintCallable, Category = "Building Info Widget")
	void ShowBuildingInfoWidget(const FString& BuildingId, const FString& BuildingData);
	
	UFUNCTION(BlueprintCallable, Category = "Building Info Widget") 
	void HideBuildingInfoWidget();
	
	UFUNCTION(BlueprintCallable, Category = "Building Info Widget")
	void CreateBuildingInfoWidget();
	
	UFUNCTION(BlueprintCallable, Category = "Cache Statistics")
	void LogCacheStatistics();

	void CreateBuildingAttributesForm(const FString& JsonData = TEXT(""));
	
	void PopulateBuildingAttributesWidget(const FString& JsonData);

	void GetBuildingAttributeOptions();
	
	UFUNCTION(BlueprintCallable, Category = "Building Attributes")
	void OnBuildingClicked(const FString& BuildingGmlId);
	
	// Enhanced Building Click Handler with Position Validation
	UFUNCTION(BlueprintCallable, Category = "Building Interaction")
	void OnBuildingClickedWithPosition(const FString& BuildingGmlId, const FVector& ClickPosition);
	
	UFUNCTION(BlueprintCallable, Category = "Building Attributes")
	void CloseAttributesForm();
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	TSubclassOf<class UUserWidget> BuildingAttributesWidgetClass;
	
	UPROPERTY(BlueprintReadOnly, Category = "UI")
	class UUserWidget* BuildingAttributesWidget;
	
	void CreateMultipleMaterialsForCesium(AActor* CesiumActor);
	
	FString CreateCesiumColorExpression();
	
	void CreateTestColors();

private:
	FLinearColor ConvertHexToLinearColor(const FString& HexColor);

	FString ConvertGmlIdToBuildingKey(const FString& GmlId);
	
	FString ConvertActualGmlIdToModified(const FString& ActualGmlId);

	void OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void OnPreloadResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void OnAuthResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	
	void OnRefreshTokenResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	
	// REST API Energy Polling Functions
	void FetchUpdatedEnergyData(); // Fetch fresh energy data using REST API
	void OnEnergyUpdateResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void ParseAndCacheAllBuildings(const FString& JsonResponse);
	
	void OnGetBuildingAttributesResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void OnUpdateBuildingAttributesResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	
	void OnRealTimeEnergyDataResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	TMap<FString, FString> BuildingDataCache;
	
	TMap<FString, FLinearColor> BuildingColorCache;

	TMap<FString, FString> GmlIdCache;
	
	FString CurrentRequestedBuildingKey;
	FString CurrentRequestedCommunityId;

	bool bDataLoaded;

	bool bIsLoading;
	
	float RealTimeMonitoringTimer = 0.0f;
	float RealTimeUpdateInterval = 2.0f;
	
	bool bEnhancedPollingMode = true;
	float FastPollingInterval = 1.0f;
	float SlowPollingInterval = 5.0f;
	int32 NoChangesCount = 0;
	
	// WebSocket Real-Time Energy Variables
	TSharedPtr<IWebSocket> EnergyWebSocket;
	bool bEnergyWebSocketConnected = false;
	FString EnergyWebSocketURL;
	float WebSocketReconnectTimer = 0.0f;
	float WebSocketReconnectInterval = 5.0f;
	bool bAutoReconnectWebSocket = true;
	bool bAuthenticationMessageShown = false; // Flag to prevent authentication spam
	int32 EnergyUpdateCounter = 0;
	
	// Token Management Variables
	FString RefreshToken; // Store refresh token for automatic token renewal
	
	// Coordinate-Based Building Validation Variables
	TMap<FString, TArray<FVector>> BuildingCoordinatesCache; // Cache of building coordinates for validation
	TMap<FString, FString> CoordinateToGmlIdMap; // Map coordinates to correct gml_id
	float CoordinateValidationTolerance = 10.0f; // Tolerance for coordinate matching in meters
	int32 SlowDownThreshold = 10;
	
	TMap<FString, FString> PreviousBuildingDataSnapshot;
	TMap<FString, FLinearColor> PreviousColorSnapshot;
	
	bool bRealTimeMonitoringEnabled = true;
	bool bIsPerformingRealTimeUpdate = false;
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time")
	void StartRealTimeMonitoring();
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time")
	void StopRealTimeMonitoring();
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time")
	void SetRealTimeUpdateInterval(float Seconds);
	
	UFUNCTION(BlueprintCallable, Category = "Real-Time")
	void EnableEnhancedPolling(bool bEnable);
	
	void PerformRealTimeDataCheck();
	void OnRealTimeDataResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void DetectAndApplyChanges(const FString& NewJsonData);
	void NotifyRealTimeChanges(const TArray<FString>& ChangedBuildings);
	void UpdatePollingStrategy(bool bChangesDetected);
	
	float CacheRefreshTimer = 0.0f;
	
	FString LastDisplayedGmlId;
	double LastDisplayTime;
	
	FString CurrentBuildingGmlId;
	
	bool bShowScreenMessages; // Control whether to show on-screen debug messages
};
