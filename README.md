# Real-Time Building Energy Visualization in Mixed Reality

## Overview

This project implements a real-time building energy visualization system using Unreal Engine 5.6 with Cesium 3D tilesets integration, extending from desktop development to mixed reality deployment on HoloLens 2. The system demonstrates building energy consumption through color-coded 3D visualizations with a case study in Bisingen, Germany.

## 🚀 Quick Start

### Prerequisites

- **Unreal Engine 5.6** or later
- **Visual Studio 2022** with C++ development tools
- **Cesium for Unreal plugin** (install from UE Marketplace)
- **HoloLens 2** (optional, for mixed reality deployment)
- Access to backend API (backend.gisworld-tech.com)

### Setup Instructions

1. **Clone the Repository**
   ```bash
   git clone https://github.com/Cephas2374/thesis_c-.git
   cd thesis_c-
   ```

2. **Generate Project Files**
   - Right-click on `Final_project.uproject`
   - Select "Generate Visual Studio project files"

3. **Install Cesium Plugin**
   - Open Epic Games Launcher
   - Go to Unreal Engine → Library → Vault
   - Install "Cesium for Unreal" plugin
   - Enable plugin in Project Settings

4. **Build the Project**
   ```bash
   # Using MSBuild
   "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Final_project.sln /p:Configuration="Development Editor" /p:Platform=Win64
   ```

5. **Configure API Access**
   - Open the project in Unreal Editor
   - Place `ABuildingEnergyDisplay` actor in your level
   - Set `AccessToken` and `RefreshToken` in the actor details panel

## 📁 Project Structure

```
Final_project/
├── Source/                          # C++ source code
│   ├── Final_project/
│   │   ├── BuildingEnergyDisplay.*  # Main energy visualization actor
│   │   ├── HoloLensInputConverter.* # Mixed reality input handling
│   │   ├── BuildingAttributesWidget.* # UI widget for building data
│   │   └── Final_project.Build.cs   # Module dependencies
│   ├── Final_project.Target.cs      # Build target configuration
│   └── Final_projectEditor.Target.cs
├── Content/                         # Game assets and blueprints
│   ├── BP_3D/                       # Blueprint implementations
│   ├── UMG/                         # User interface widgets
│   │   └── WBP_BuildingAttributes.uasset
│   ├── Materials/                   # Cesium materials
│   ├── Level/                       # Level maps
│   └── CesiumSettings/              # Cesium configuration
├── Config/                          # Project configuration files
├── Docs/                            # Documentation
│   ├── METHODOLOGY_TECHNICAL.md     # Technical methodology
│   ├── IMPLEMENTATION_GUIDE.md      # Step-by-step implementation
│   └── METHODS_AND_SOLUTIONS_ANALYSIS.md
└── Final_project.uproject           # Unreal Engine project file
```

## 🏗️ Core Components

### BuildingEnergyDisplay (C++)
- **Purpose**: Main actor for building energy visualization
- **Features**: 
  - JWT authentication with automatic token refresh
  - Real-time API polling for building energy data
  - Multi-layered caching system
  - Cesium tileset color application

### HoloLensInputConverter (C++)
- **Purpose**: Cross-platform input handling for HoloLens 2
- **Features**:
  - Mouse click to gesture conversion (air tap, tap-and-hold)
  - Platform detection and input abstraction
  - Gaze-based interaction for mixed reality

### WBP_BuildingAttributes (UMG)
- **Purpose**: User interface for building attribute display and editing
- **Features**:
  - Real-time building data visualization
  - Interactive form controls for data modification
  - Responsive design for desktop and mixed reality

## 🔧 Configuration

### API Configuration
```cpp
// In BuildingEnergyDisplay actor
BaseURL = "https://backend.gisworld-tech.com"
AccessToken = "your_access_token_here"
RefreshToken = "your_refresh_token_here"
```

### HoloLens 2 Deployment
1. **Enable HoloLens Platform Support**
   - Project Settings → Platforms → HoloLens
   - Set Target Device Family to "Windows.Holographic"

2. **Configure Mixed Reality Settings**
   - Enable "Start in VR" in Project Settings → XR
   - Set Default XR System to "Windows Mixed Reality"

3. **Build for HoloLens**
   ```bash
   MSBuild Final_project.sln /p:Configuration="Shipping" /p:Platform="HoloLens"
   ```

## 📚 Documentation

- **[METHODOLOGY_TECHNICAL.md](METHODOLOGY_TECHNICAL.md)**: Complete technical methodology and architectural decisions
- **[IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md)**: Step-by-step implementation guide with code examples
- **[METHODS_AND_SOLUTIONS_ANALYSIS.md](METHODS_AND_SOLUTIONS_ANALYSIS.md)**: Comparative analysis of methodologies and solutions

## 🎯 Key Features

- **Real-time Building Energy Visualization**: Color-coded 3D buildings based on energy consumption
- **Cross-platform Deployment**: Desktop and HoloLens 2 mixed reality support
- **Interactive Building Selection**: Click/tap to view detailed building attributes
- **Cesium 3D Tileset Integration**: High-performance geospatial visualization
- **Robust Authentication**: JWT-based API security with automatic refresh
- **Case Study Implementation**: Bisingen, Germany municipal energy data

## 🔍 Troubleshooting

### Common Issues

1. **Cesium Color Visualization Problems**
   - Ensure CesiumFeaturesMetadata component is attached to tileset
   - Use Auto Fill to discover metadata properties
   - Check GML ID mapping between API and tileset

2. **Authentication Failures**
   - Verify access and refresh tokens
   - Check network connectivity
   - For development: Add `http.sslverification=false` to DefaultEngine.ini

3. **Build Errors**
   - Regenerate project files
   - Clean and rebuild solution
   - Verify all required modules in Final_project.Build.cs

### Performance Optimization

- **Desktop**: 30-second API polling interval
- **HoloLens 2**: 60-second polling interval for battery conservation
- **Rendering**: Automatic quality adjustment based on platform

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📄 License

This project is part of academic research. Please cite appropriately if used in academic work.

## 📧 Contact

- **Author**: Cephas
- **Repository**: [https://github.com/Cephas2374/thesis_c-](https://github.com/Cephas2374/thesis_c-)
- **Documentation**: See `/Docs` folder for comprehensive technical documentation

---

*This project demonstrates the integration of geospatial data standards, modern web technologies, game engine capabilities, and emerging mixed reality platforms to create a comprehensive building energy visualization ecosystem.*