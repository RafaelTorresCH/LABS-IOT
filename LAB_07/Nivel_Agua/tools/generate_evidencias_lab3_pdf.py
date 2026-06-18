from pathlib import Path
import re

from PIL import Image
from reportlab.lib.pagesizes import landscape, letter
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


ROOT = Path(__file__).resolve().parents[1]
IMAGES_DIR = ROOT / "evidenciaslab3"
OUTPUT_PDF = ROOT / "Evidencias_Laboratorio3.pdf"

PAGE_SIZE = landscape(letter)
PAGE_W, PAGE_H = PAGE_SIZE
MARGIN = 30
TITLE_Y = PAGE_H - 34
CAPTION_H = 42


def sort_key(path: Path) -> tuple[int, str]:
    match = re.match(r"\s*(\d+)", path.stem)
    return (int(match.group(1)) if match else 9999, path.name.lower())


def title_from_filename(path: Path) -> str:
    return path.stem.strip()


def fit_size(image_w: int, image_h: int, max_w: float, max_h: float) -> tuple[float, float]:
    scale = min(max_w / image_w, max_h / image_h)
    return image_w * scale, image_h * scale


def build_pdf() -> None:
    images = sorted(
        [*IMAGES_DIR.glob("*.jpeg"), *IMAGES_DIR.glob("*.jpg"), *IMAGES_DIR.glob("*.png")],
        key=sort_key,
    )
    if not images:
        raise SystemExit(f"No evidence images found in {IMAGES_DIR}")

    pdf = canvas.Canvas(str(OUTPUT_PDF), pagesize=PAGE_SIZE)
    pdf.setTitle("Evidencias Laboratorio 3 - CoAP y CBOR")
    pdf.setAuthor("GreenField Technologies - SoilSense")

    for index, image_path in enumerate(images, start=1):
        caption = title_from_filename(image_path)
        with Image.open(image_path) as image:
            image_w, image_h = image.size

        pdf.setFillColorRGB(0.08, 0.12, 0.16)
        pdf.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)

        pdf.setFillColorRGB(1, 1, 1)
        pdf.setFont("Helvetica-Bold", 15)
        pdf.drawString(MARGIN, TITLE_Y, "Evidencias Laboratorio 3 - CoAP / CBOR")
        pdf.setFont("Helvetica", 9)
        pdf.drawRightString(PAGE_W - MARGIN, TITLE_Y + 2, f"Pagina {index} de {len(images)}")

        image_top = TITLE_Y - 18
        image_bottom = MARGIN + CAPTION_H
        max_w = PAGE_W - 2 * MARGIN
        max_h = image_top - image_bottom
        draw_w, draw_h = fit_size(image_w, image_h, max_w, max_h)
        image_x = (PAGE_W - draw_w) / 2
        image_y = image_bottom + (max_h - draw_h) / 2

        pdf.setStrokeColorRGB(0.32, 0.55, 0.66)
        pdf.setLineWidth(1)
        pdf.rect(image_x - 1, image_y - 1, draw_w + 2, draw_h + 2, fill=0, stroke=1)
        pdf.drawImage(
            ImageReader(str(image_path)),
            image_x,
            image_y,
            width=draw_w,
            height=draw_h,
            preserveAspectRatio=True,
            mask="auto",
        )

        pdf.setFillColorRGB(0.12, 0.18, 0.23)
        pdf.roundRect(MARGIN, MARGIN, PAGE_W - 2 * MARGIN, 28, 5, fill=1, stroke=0)
        pdf.setFillColorRGB(1, 1, 1)
        pdf.setFont("Helvetica-Bold", 10)
        pdf.drawCentredString(PAGE_W / 2, MARGIN + 10, caption)
        pdf.showPage()

    pdf.save()
    print(OUTPUT_PDF)


if __name__ == "__main__":
    build_pdf()
