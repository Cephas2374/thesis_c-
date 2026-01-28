import fitz  # PyMuPDF
import os
import re
from pathlib import Path
from PIL import Image
import numpy as np
import cv2

def extract_images_from_pdf(pdf_path, output_dir, paper_category="", max_images=3):
    """
    Extract images from PDF and save as PNG files
    """
    extracted_images = []
    
    try:
        pdf_document = fitz.open(pdf_path)
        pdf_name = Path(pdf_path).stem
        
        print(f"Processing: {pdf_name}")
        
        image_count = 0
        for page_num in range(min(len(pdf_document), 15)):  # Limit to first 15 pages to focus on key content
            page = pdf_document[page_num]
            image_list = page.get_images()
            
            for img_index, img in enumerate(image_list):
                if image_count >= max_images:
                    break
                    
                # Extract image
                xref = img[0]
                pix = fitz.Pixmap(pdf_document, xref)
                
                # Skip very small images (likely icons or decorative elements)
                if pix.width < 200 or pix.height < 150:
                    pix = None
                    continue
                
                # Convert to PNG
                if pix.n - pix.alpha < 4:  # GRAY or RGB
                    # Generate descriptive filename based on content and category
                    if "cityxml" in pdf_name.lower() or "citygml" in pdf_name.lower():
                        img_name = f"cityxml_schema_diagram_{image_count + 1}.png"
                    elif "cesium" in pdf_name.lower() or "rendering" in paper_category.lower():
                        img_name = f"cesium_rendering_pipeline_{image_count + 1}.png"
                    elif "restful" in paper_category.lower() or "api" in pdf_name.lower():
                        img_name = f"restful_api_architecture_{image_count + 1}.png"
                    elif "hololens" in paper_category.lower() or "ar" in pdf_name.lower():
                        img_name = f"hololens_ar_system_{image_count + 1}.png"
                    else:
                        img_name = f"{pdf_name[:50]}_fig_{image_count + 1}.png"
                    
                    img_path = os.path.join(output_dir, img_name)
                    pix.save(img_path)
                    
                    # Check image quality and relevance
                    if is_relevant_diagram(img_path):
                        extracted_images.append({
                            'filename': img_name,
                            'source_paper': pdf_name,
                            'page': page_num + 1,
                            'category': paper_category,
                            'path': img_path
                        })
                        image_count += 1
                        print(f"  Extracted: {img_name} from page {page_num + 1}")
                    else:
                        # Remove irrelevant image
                        os.remove(img_path)
                
                pix = None
        
        pdf_document.close()
        
    except Exception as e:
        print(f"Error processing {pdf_path}: {str(e)}")
    
    return extracted_images

def is_relevant_diagram(img_path):
    """
    Check if the extracted image is likely to be a relevant diagram/schema
    """
    try:
        img = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
        if img is None:
            return False
        
        # Check image dimensions (favor larger, diagram-like images)
        height, width = img.shape
        if width < 300 or height < 200:
            return False
        
        # Check for diagram-like characteristics (edges, geometric shapes)
        edges = cv2.Canny(img, 50, 150)
        edge_density = np.sum(edges > 0) / (width * height)
        
        # Diagrams typically have moderate edge density (not too sparse, not too dense like photos)
        if 0.02 < edge_density < 0.3:
            return True
        
        # Check for text content (diagrams often contain labels)
        # Simple heuristic: if image has good contrast and structured content
        mean_val = np.mean(img)
        std_val = np.std(img)
        
        # Good diagrams have reasonable contrast
        if std_val > 30 and 50 < mean_val < 200:
            return True
        
        return False
    
    except:
        return False

def process_research_papers():
    """
    Main function to process papers from different categories
    """
    base_dir = Path("d:/ar_thesis_cephas/unreal/Final_project/latex")
    output_dir = base_dir / "png"
    output_dir.mkdir(exist_ok=True)
    
    # Define categories and their corresponding directories
    categories = {
        "CityGML Schema Design": "CityGML-based schema design in spatial databases_20260113_114822/pdfs",
        "Cesium Rendering": "CesiumJS Rendering pipelines_20260113_031325/pdfs", 
        "RESTful APIs": "RESTful and real-time APIs in geospatial platforms_20260113_031515/pdfs",
        "HoloLens AR": "HoloLens 2 technology and integration in the Unreal Engine_20260113_031302/pdfs"
    }
    
    # Priority papers based on titles
    priority_papers = [
        "A Prototype Architecture for Interactive 3D Maps on the Web.pdf",
        "Data structuring & interoperability options for optimising 3D city modelling.pdf",
        "Development of a microservices-based cloud platform for real-time satellite data capture and analysi.pdf",
        "Real-time GIS Programming and Geocomputation.pdf",
        "Geospatial information processing technologies.pdf"
    ]
    
    all_extracted = []
    
    for category, subdir in categories.items():
        category_dir = base_dir / subdir
        if not category_dir.exists():
            continue
        
        print(f"\n=== Processing {category} ===")
        
        # Process priority papers first
        for priority_paper in priority_papers:
            paper_path = category_dir / priority_paper
            if paper_path.exists():
                images = extract_images_from_pdf(str(paper_path), str(output_dir), category, max_images=2)
                all_extracted.extend(images)
        
        # Then process other relevant papers (limit to avoid too many images)
        other_papers = [f for f in category_dir.glob("*.pdf") if f.name not in priority_papers]
        for i, paper_path in enumerate(other_papers[:3]):  # Limit to 3 additional papers per category
            images = extract_images_from_pdf(str(paper_path), str(output_dir), category, max_images=1)
            all_extracted.extend(images)
    
    return all_extracted

def generate_report(extracted_images):
    """
    Generate a summary report of extracted images
    """
    report = []
    report.append("# Extracted Figures Report\n")
    
    # Group by category
    by_category = {}
    for img in extracted_images:
        cat = img['category']
        if cat not in by_category:
            by_category[cat] = []
        by_category[cat].append(img)
    
    for category, images in by_category.items():
        report.append(f"## {category}\n")
        for img in images:
            report.append(f"- **{img['filename']}**")
            report.append(f"  - Source: {img['source_paper']}")
            report.append(f"  - Page: {img['page']}")
            report.append(f"  - Category: {img['category']}")
            
            # Suggest literature review section
            if "cityxml" in img['filename'].lower() or "citygml" in img['filename'].lower():
                section = "Section 2.2: CityGML and 3D Urban Modeling"
            elif "cesium" in img['filename'].lower() or "rendering" in img['filename'].lower():
                section = "Section 2.3: Cesium and 3D Tiles Ecosystem"
            elif "restful" in img['filename'].lower() or "api" in img['filename'].lower():
                section = "Section 2.1: RESTful APIs and Real-time Communication"
            elif "hololens" in img['filename'].lower() or "ar" in img['filename'].lower():
                section = "Section 2.4: WebXR and Augmented Reality Applications"
            else:
                section = "Section 2.6: Integration Challenges and Performance Optimization"
            
            report.append(f"  - Suggested section: {section}")
            report.append("")
    
    return "\n".join(report)

if __name__ == "__main__":
    print("Starting figure extraction process...")
    extracted = process_research_papers()
    
    print(f"\n=== Extraction Complete ===")
    print(f"Total images extracted: {len(extracted)}")
    
    # Generate report
    report = generate_report(extracted)
    
    # Save report
    report_path = "d:/ar_thesis_cephas/unreal/Final_project/latex/extracted_figures_report.md"
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write(report)
    
    print(f"Report saved to: {report_path}")
    print("\n" + report)