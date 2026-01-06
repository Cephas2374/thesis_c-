# Git Upload Guide for Building Energy Visualization Project

## Files to Upload to GitHub

### Essential Project Files (MUST INCLUDE)

#### 1. Project Configuration
- `Final_project.uproject` - Main Unreal Engine project file
- `Final_project.sln` - Visual Studio solution file
- `README.md` - Project documentation (created above)

#### 2. Source Code (ALL REQUIRED)
- `Source/Final_project.Target.cs`
- `Source/Final_projectEditor.Target.cs`
- `Source/Final_project/Final_project.Build.cs`
- `Source/Final_project/Final_project.cpp`
- `Source/Final_project/Final_project.h`
- `Source/Final_project/BuildingEnergyDisplay.cpp`
- `Source/Final_project/BuildingEnergyDisplay.h`
- `Source/Final_project/HoloLensInputConverter.cpp`
- `Source/Final_project/HoloLensInputConverter.h`
- `Source/Final_project/BuildingAttributesWidget.cpp`
- `Source/Final_project/BuildingAttributesWidget.h`

#### 3. Configuration Files
- `Config/DefaultEngine.ini`
- `Config/DefaultGame.ini`
- `Config/DefaultInput.ini`
- `Config/DefaultEditor.ini`

#### 4. Content Assets (ESSENTIAL)
- `Content/UMG/WBP_BuildingAttributes.uasset` - UI widget
- `Content/BP_3D/` folder (all blueprint files)
- `Content/Level/` folder (level files)
- `Content/Materials/` folder (material assets)
- `Content/CesiumSettings/` folder

#### 5. Documentation (ALL THREE)
- `METHODOLOGY_TECHNICAL.md`
- `IMPLEMENTATION_GUIDE.md`
- `METHODS_AND_SOLUTIONS_ANALYSIS.md`

### Files to EXCLUDE (Add to .gitignore)

#### Large/Generated Files
- `Binaries/` - Generated executable files
- `Intermediate/` - Temporary build files
- `DerivedDataCache/` - Derived data cache
- `Saved/` - Saved game data and logs
- `.vs/` - Visual Studio cache
- `.vscode/` - VS Code settings
- `*.tmp` - Temporary files
- `cesium-request-cache.sqlite*` - Cesium cache files

## .gitignore File

Create a `.gitignore` file in your project root:

```gitignore
# Unreal Engine
Binaries/
Intermediate/
DerivedDataCache/
Saved/

# Visual Studio
.vs/
*.opensdf
*.sdf
*.sln.docstates
*.userprefs
*.pidb
*.booproj
*.svd
*.pdb
*.idb
*.ilk
*.meta
*.obj
*.iobj
*.pch
*.pdb
*.ipdb
*.pgc
*.pgd
*.rsp
*.sbr
*.tlb
*.tli
*.tlh
*.tmp
*.tmp_proj
*.log
*.vspscc
*.vssscc
.builds
*.pidb
*.svclog
*.scc

# VS Code
.vscode/

# Cesium Cache
cesium-request-cache.sqlite*

# OS
.DS_Store
Thumbs.db
Desktop.ini

# Temporary
*.tmp
*.temp
*~
```

## Git Upload Commands

1. **Initialize Git Repository** (if not already done):
   ```bash
   cd d:\ar_thesis_cephas\unreal\Final_project
   git init
   git remote add origin https://github.com/Cephas2374/thesis_c-.git
   ```

2. **Create .gitignore**:
   ```bash
   # Create the .gitignore file with content above
   ```

3. **Add Essential Files**:
   ```bash
   git add Final_project.uproject
   git add Final_project.sln
   git add README.md
   git add Source/
   git add Config/
   git add Content/UMG/
   git add Content/BP_3D/
   git add Content/Level/
   git add Content/Materials/
   git add Content/CesiumSettings/
   git add *.md  # All documentation files
   git add .gitignore
   ```

4. **Commit and Push**:
   ```bash
   git commit -m "Initial commit: Building Energy Visualization system with HoloLens 2 support"
   git branch -M main
   git push -u origin main
   ```

## Repository Structure After Upload

```
thesis_c-/
├── README.md
├── .gitignore
├── Final_project.uproject
├── Final_project.sln
├── Source/                    # C++ source code
├── Config/                    # Project configuration
├── Content/                   # Essential game assets
│   ├── UMG/                  # UI widgets
│   ├── BP_3D/                # Blueprints
│   ├── Level/                # Levels
│   ├── Materials/            # Materials
│   └── CesiumSettings/       # Cesium config
├── METHODOLOGY_TECHNICAL.md  # Technical documentation
├── IMPLEMENTATION_GUIDE.md   # Implementation guide
└── METHODS_AND_SOLUTIONS_ANALYSIS.md  # Analysis document
```

## Size Optimization Tips

1. **Exclude large binaries** - Use .gitignore properly
2. **Compress assets** - UE assets are already compressed
3. **Documentation only** - Focus on code and essential assets
4. **No build outputs** - Never commit Binaries/ or Intermediate/

This setup will provide everything needed to recreate the project on another computer while keeping the repository size manageable.