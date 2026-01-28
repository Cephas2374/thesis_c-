import fitz  # PyMuPDF
import os
import re
from pathlib import Path
from PIL import Image
import numpy as np
import cv2
import hashlib

def extract_images_from_pdf_improved(pdf_path, output_dir, paper_category="", max_images=2):
    """
    Extract images from PDF with improved naming and quality filtering
    """
    extracted_images = []
    
    try:
        pdf_document = fitz.open(pdf_path)
        pdf_name = Path(pdf_path).stem
        
        print(f"Processing: {pdf_name}")
        
        image_count = 0
        for page_num in range(min(len(pdf_document), 20)):  # Check more pages
            page = pdf_document[page_num]
            image_list = page.get_images()
            
            for img_index, img in enumerate(image_list):
                if image_count >= max_images:
                    break
                    
                # Extract image
                xref = img[0]
                pix = fitz.Pixmap(pdf_document, xref)
                
                # Skip very small images
                if pix.width < 250 or pix.height < 200:
                    pix = None
                    continue
                
                # Convert to PNG and check quality
                if pix.n - pix.alpha < 4:  # GRAY or RGB
                    # Create temporary file to check quality
                    temp_path = os.path.join(output_dir, f"temp_{page_num}_{img_index}.png")
                    pix.save(temp_path)
                    
                    # Check if image is relevant
                    if is_relevant_technical_diagram(temp_path):
                        # Generate unique, descriptive filename
                        img_name = generate_descriptive_filename(pdf_name, paper_category, page_num, image_count)
                        final_path = os.path.join(output_dir, img_name)
                        
                        # Rename from temp to final name
                        os.rename(temp_path, final_path)
                        
                        extracted_images.append({
                            'filename': img_name,
                            'source_paper': pdf_name,
                            'page': page_num + 1,
                            'category': paper_category,
                            'path': final_path,
                            'dimensions': f"{pix.width}x{pix.height}"
                        })
                        image_count += 1
                        print(f"  ✓ Extracted: {img_name} from page {page_num + 1} ({pix.width}x{pix.height})")
                    else:
                        # Remove temp file if not relevant
                        os.remove(temp_path)
                
                pix = None
        
        pdf_document.close()
        
    except Exception as e:
        print(f"Error processing {pdf_path}: {str(e)}")
    
    return extracted_images

def generate_descriptive_filename(pdf_name, category, page_num, img_count):
    """
    Generate descriptive filenames based on content analysis
    """
    pdf_lower = pdf_name.lower()
    
    # Base name patterns
    if "prototype" in pdf_lower and "3d maps" in pdf_lower:
        base = "3d_web_maps_architecture"
    elif "data structuring" in pdf_lower and "3d city" in pdf_lower:
        base = "cityxml_data_structuring_schema"
    elif "microservices" in pdf_lower and "satellite" in pdf_lower:
        base = "microservices_satellite_api_architecture"
    elif "real-time gis" in pdf_lower:
        base = "realtime_gis_processing_workflow"
    elif "geospatial" in pdf_lower and "processing" in pdf_lower:
        base = "geospatial_processing_framework"
    elif category == "CityGML Schema Design":
        base = f"citygml_schema_model_{img_count + 1}"
    elif category == "Cesium Rendering":
        base = f"cesium_3dtiles_pipeline_{img_count + 1}"
    elif category == "RESTful APIs":
        base = f"restful_api_architecture_{img_count + 1}"
    elif category == "HoloLens AR":
        base = f"hololens_ar_framework_{img_count + 1}"
    else:
        # Create hash for uniqueness
        hash_obj = hashlib.md5(f"{pdf_name}_{page_num}_{img_count}".encode())
        base = f"technical_diagram_{hash_obj.hexdigest()[:8]}"
    
    return f"{base}.png"

def is_relevant_technical_diagram(img_path):
    """
    Enhanced function to identify technical diagrams, architectures, and schemas
    """
    try:
        # Load image in both color and grayscale
        img_color = cv2.imread(img_path)
        img_gray = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
        
        if img_color is None or img_gray is None:
            return False
        
        height, width = img_gray.shape
        
        # Skip very small or very large images
        if width < 250 or height < 200 or width > 2000 or height > 2000:
            return False
        
        # Check for diagram-like characteristics
        edges = cv2.Canny(img_gray, 30, 100)
        edge_density = np.sum(edges > 0) / (width * height)
        
        # Check for geometric shapes (rectangles, circles, lines)
        contours, _ = cv2.findContours(edges, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        
        # Count rectangular and geometric shapes
        rect_count = 0
        for contour in contours:
            area = cv2.contourArea(contour)
            if area > 100:  # Minimum area threshold
                # Approximate contour
                epsilon = 0.02 * cv2.arcLength(contour, True)
                approx = cv2.approxPolyDP(contour, epsilon, True)
                
                # Check if it's rectangular (4 vertices) or geometric
                if len(approx) >= 4:
                    rect_count += 1
        
        # Check color characteristics (diagrams often have limited color palette)
        img_hsv = cv2.cvtColor(img_color, cv2.COLOR_BGR2HSV)
        unique_colors = len(np.unique(img_hsv.reshape(-1, img_hsv.shape[2]), axis=0))
        
        # Check contrast and text-like regions
        mean_val = np.mean(img_gray)
        std_val = np.std(img_gray)
        
        # Scoring system for diagram likelihood
        score = 0
        
        # Good edge density (not too sparse, not too dense like photos)
        if 0.01 < edge_density < 0.25:
            score += 3
        
        # Presence of geometric shapes
        if rect_count > 2:
            score += 3
        
        # Good contrast (diagrams usually have good contrast)
        if std_val > 25:
            score += 2
        
        # Reasonable mean brightness (not too dark or too bright)
        if 40 < mean_val < 220:
            score += 1
        
        # Limited color palette (typical of diagrams)
        if unique_colors < 1000:
            score += 2
        
        # Check aspect ratio (diagrams often have reasonable aspect ratios)
        aspect_ratio = width / height
        if 0.5 < aspect_ratio < 3.0:
            score += 1
        
        # Return True if score is high enough
        return score >= 6
        
    except Exception as e:
        print(f"Error analyzing image {img_path}: {e}")
        return False

def process_key_papers_only():
    """
    Process only the most relevant papers for each category
    """
    base_dir = Path("d:/ar_thesis_cephas/unreal/Final_project/latex")
    output_dir = base_dir / "png"
    output_dir.mkdir(exist_ok=True)
    
    # Clear existing images to avoid conflicts
    for existing_file in output_dir.glob("*.png"):
        if existing_file.name != "temp.png":  # Keep any temp files for now
            existing_file.unlink()
    
    # Define key papers for each category
    key_papers = {
        "CityGML Schema Design": (
            "CityGML-based schema design in spatial databases_20260113_114822/pdfs", 
             ["A Prototype Architecture for Interactive 3D Maps on the Web.pdf",
              "Data structuring & interoperability options for optimising 3D city modelling.pdf"]
        ),
        "Cesium Rendering": (
            "CesiumJS Rendering pipelines_20260113_031325/pdfs", 
             ["A Prototype Architecture for Interactive 3D Maps on the Web.pdf"]
        ),
        "RESTful APIs": (
            "RESTful and real-time APIs in geospatial platforms_20260113_031515/pdfs", 
             ["Development of a microservices-based cloud platform for real-time satellite data capture and analysi.pdf",
              "Real-time GIS Programming and Geocomputation.pdf",
              "Geospatial information processing technologies.pdf"]
        ),
        "HoloLens AR": (
            "HoloLens 2 technology and integration in the Unreal Engine_20260113_031302/pdfs", 
             []  # Will process the first few arxiv papers
        )
    }
    
    all_extracted = []
    
    for category, (subdir, specific_papers) in key_papers.items():
        category_dir = base_dir / subdir
        if not category_dir.exists():
            continue
        
        print(f"\n=== Processing {category} ===")
        
        # Process specific papers
        for paper_name in specific_papers:
            paper_path = category_dir / paper_name
            if paper_path.exists():
                images = extract_images_from_pdf_improved(str(paper_path), str(output_dir), category, max_images=2)
                all_extracted.extend(images)
        
        # For HoloLens, process first few arxiv papers since no specific papers listed
        if category == "HoloLens AR":
            arxiv_papers = sorted([f for f in category_dir.glob("arxiv_*.pdf")])[:3]
            for paper_path in arxiv_papers:
                images = extract_images_from_pdf_improved(str(paper_path), str(output_dir), category, max_images=1)
                all_extracted.extend(images)
    
    return all_extracted

def generate_detailed_report(extracted_images):
    """
    Generate a comprehensive report with image descriptions and usage recommendations
    """
    report = []
    report.append("# Literature Review Figure Extraction Report")
    report.append(f"**Date:** {Path().cwd()}")
    report.append(f"**Total Images Extracted:** {len(extracted_images)}\n")
    
    # Group by suggested literature review section
    by_section = {
        "Section 2.1: RESTful APIs and Real-time Communication": [],
        "Section 2.2: CityGML and 3D Urban Modeling": [],
        "Section 2.3: Cesium and 3D Tiles Ecosystem": [],
        "Section 2.4: WebXR and Augmented Reality Applications": [],
        "Section 2.5: Unreal Engine and C++/Blueprint Development": [],
        "Section 2.6: Integration Challenges and Performance Optimization": []
    }
    
    for img in extracted_images:
        filename = img['filename'].lower()
        
        if 'api' in filename or 'microservices' in filename or 'restful' in filename:
            section = "Section 2.1: RESTful APIs and Real-time Communication"
        elif 'citygml' in filename or 'cityxml' in filename or '3d_city' in filename:
            section = "Section 2.2: CityGML and 3D Urban Modeling"
        elif 'cesium' in filename or '3dtiles' in filename or 'rendering' in filename:
            section = "Section 2.3: Cesium and 3D Tiles Ecosystem"
        elif 'hololens' in filename or 'ar' in filename or '3d_web_maps' in filename:
            section = "Section 2.4: WebXR and Augmented Reality Applications"
        elif 'unreal' in filename or 'ue4' in filename or 'blueprint' in filename:
            section = "Section 2.5: Unreal Engine and C++/Blueprint Development"
        else:
            section = "Section 2.6: Integration Challenges and Performance Optimization"
        
        by_section[section].append(img)
    
    # Generate section-based report
    for section, images in by_section.items():
        if images:
            report.append(f"## {section}")
            report.append("")
            for img in images:
                report.append(f"### {img['filename']}")
                report.append(f"- **Source Paper:** {img['source_paper']}")
                report.append(f"- **Page:** {img['page']}")
                report.append(f"- **Dimensions:** {img['dimensions']}")
                report.append(f"- **File Path:** `{img['path']}`")
                
                # Add description based on filename patterns
                if 'architecture' in img['filename']:
                    report.append(f"- **Description:** System architecture diagram showing component relationships and data flow")
                elif 'schema' in img['filename']:
                    report.append(f"- **Description:** Data schema or UML model showing structural relationships")
                elif 'pipeline' in img['filename']:
                    report.append(f"- **Description:** Processing pipeline or workflow diagram")
                elif 'framework' in img['filename']:
                    report.append(f"- **Description:** Technical framework or system overview")
                else:
                    report.append(f"- **Description:** Technical diagram related to {img['category']}")
                
                # Add LaTeX citation template
                paper_cite_key = img['source_paper'].replace(' ', '_').replace('.pdf', '').lower()
                report.append(f"- **LaTeX Citation:** `\\cite{{{paper_cite_key}}}`")
                report.append("")
    
    # Add usage instructions
    report.append("## Usage Instructions")
    report.append("")
    report.append("1. **File Location:** All extracted images are saved in `latex/png/`")
    report.append("2. **LaTeX Integration:** Use `\\includegraphics{png/filename.png}` in your document")
    report.append("3. **Citation:** Remember to cite the source papers using the provided citation keys")
    report.append("4. **Figure Captions:** Adapt the descriptions above for your figure captions")
    report.append("")
    report.append("## Example LaTeX Code")
    report.append("```latex")
    report.append("\\begin{figure}[htbp]")
    report.append("    \\centering")
    report.append("    \\includegraphics[width=0.8\\textwidth]{png/example_filename.png}")
    report.append("    \\caption{Caption text here \\cite{source_paper_key}}")
    report.append("    \\label{fig:example_label}")
    report.append("\\end{figure}")
    report.append("```")
    
    return "\n".join(report)

if __name__ == "__main__":
    print("Starting improved figure extraction process...")
    extracted = process_key_papers_only()
    
    print(f"\n=== Extraction Complete ===")
    print(f"Total high-quality images extracted: {len(extracted)}")
    
    # Generate detailed report
    report = generate_detailed_report(extracted)
    
    # Save report
    report_path = "d:/ar_thesis_cephas/unreal/Final_project/latex/literature_review_figures_report.md"
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write(report)
    
    print(f"Detailed report saved to: {report_path}")
    
    # Print summary
    print("\n=== EXTRACTED IMAGES SUMMARY ===")
    for img in extracted:
        print(f"✓ {img['filename']} - {img['source_paper']} (Page {img['page']})")