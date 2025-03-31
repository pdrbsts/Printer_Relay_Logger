import tkinter as tk
from tkinter import filedialog, ttk, messagebox
from PIL import Image, ImageTk
import os

# --- Configuration ---
HEADER_SIZE = 16
DEFAULT_WIDTH = 576 # Keep default, but expect user to change it
MAX_SLIDER_WIDTH = 1200

# --- Sequence to Remove ---
# Define the sequence as a hex string
# Commonly found in XPrinter data
SEQUENCE_TO_REMOVE_HEX1 = '1B4A181D76300048001800'
SEQUENCE_TO_REMOVE_HEX2 = '1B4A181D76300048001000'
try:
    # Convert hex string to bytes object
    SEQUENCE_TO_REMOVE1 = bytes.fromhex(SEQUENCE_TO_REMOVE_HEX1)
    print(f"Configured to remove sequence: {SEQUENCE_TO_REMOVE_HEX1}")
    SEQUENCE_TO_REMOVE2 = bytes.fromhex(SEQUENCE_TO_REMOVE_HEX2)
    print(f"Configured to remove sequence: {SEQUENCE_TO_REMOVE_HEX2}")
except ValueError:
    messagebox.showerror("Config Error", f"Invalid hex sequence provided: {SEQUENCE_TO_REMOVE_HEX1}")
    messagebox.showerror("Config Error", f"Invalid hex sequence provided: {SEQUENCE_TO_REMOVE_HEX2}")
    SEQUENCE_TO_REMOVE1 = None # Disable removal if invalid
    SEQUENCE_TO_REMOVE2 = None # Disable removal if invalid

# --- Core Logic (Modified load_and_process_bitmap) ---
def load_and_process_bitmap(filename, width, msb_first=True, invert_polarity=False):
    """
    Reads bitmap, REMOVES specific sequence, processes bits, returns PIL Image.
    """
    global SEQUENCE_TO_REMOVE1 # Access the global constant
    global SEQUENCE_TO_REMOVE2 # Access the global constant

    if not filename or not os.path.exists(filename): return None, 0, 0
    if width <= 0: messagebox.showerror("Error", "Width must be positive."); return None, 0, 0

    bytes_removed_count = 0 # Track removed bytes

    try:
        with open(filename, 'rb') as f:
            header_data = f.read(HEADER_SIZE)
            if len(header_data) < HEADER_SIZE: messagebox.showerror("Error", f"Header too short."); return None, 0, 0

            # Read the raw pixel data
            pixel_data_raw = f.read()

        if not pixel_data_raw:
            messagebox.showwarning("Warning", "No pixel data found after the header.")
            return None, width, 0

        # --- Remove the specified sequence ---
        pixel_data = pixel_data_raw
        if SEQUENCE_TO_REMOVE1: # Only attempt removal if sequence is valid
            original_len = len(pixel_data)
            pixel_data = pixel_data.replace(SEQUENCE_TO_REMOVE1, b'') # Replace with empty bytes
            bytes_removed_count = original_len - len(pixel_data)
            if bytes_removed_count > 0:
                 # Provide feedback that data was removed (useful for debugging)
                 num_instances = bytes_removed_count // len(SEQUENCE_TO_REMOVE1)
                 print(f"Removed {num_instances} instance(s) of the sequence ({bytes_removed_count} bytes removed).")
            # else:
            #    print("Sequence not found in pixel data.") # Optional debug

        if SEQUENCE_TO_REMOVE2: # Only attempt removal if sequence is valid
            original_len = len(pixel_data)
            pixel_data = pixel_data.replace(SEQUENCE_TO_REMOVE2, b'') # Replace with empty bytes
            bytes_removed_count = original_len - len(pixel_data)
            if bytes_removed_count > 0:
                 # Provide feedback that data was removed (useful for debugging)
                 num_instances = bytes_removed_count // len(SEQUENCE_TO_REMOVE2)
                 print(f"Removed {num_instances} instance(s) of the sequence ({bytes_removed_count} bytes removed).")
            # else:
            #    print("Sequence not found in pixel data.") # Optional debug

        # --- Proceed with processing the MODIFIED pixel_data ---
        if not pixel_data:
            messagebox.showwarning("Warning", "No pixel data remaining after sequence removal.")
            return None, width, 0

        pixels, total_bits = [], 0
        for byte in pixel_data: # Iterate through the MODIFIED data
            bit_range = range(7, -1, -1) if msb_first else range(8)
            for i in bit_range:
                bit = (byte >> i) & 1
                pixel_value = (255 if bit == 1 else 0) if invert_polarity else (0 if bit == 1 else 255)
                pixels.append(pixel_value)
                total_bits += 1

        if width == 0: messagebox.showerror("Error", "Width zero."); return None, 0, 0

        height = total_bits // width
        if height == 0 and total_bits > 0: height = 1

        num_pixels_to_use = width * height

        if num_pixels_to_use == 0:
             # Check if total_bits is also 0
             if total_bits == 0:
                 messagebox.showwarning("Warning", "No pixel data available to display.")
             else:
                 messagebox.showwarning("Warning", f"Calculated image size is zero (W:{width}, H:{height}) with {total_bits} bits available. Try adjusting width.")
             return None, width, height

        img = Image.new('L', (width, height))
        img.putdata(pixels[:num_pixels_to_use])

        # Pass back the count of removed bytes for status display maybe?
        return img, width, height, bytes_removed_count

    except FileNotFoundError: messagebox.showerror("Error", f"Not found: {filename}"); return None, 0, 0, 0
    except Exception as e: messagebox.showerror("Error", f"Processing failed: {e}"); return None, 0, 0, 0


class BitmapViewerApp:
    _update_job = None

    def __init__(self, root):
        self.root = root
        self.root.title("Print Data Viewer")
        self.root.minsize(450, 300)
        self.current_file = tk.StringVar()
        self.image_width = tk.IntVar(value=DEFAULT_WIDTH)
        self.image_height = tk.IntVar(value=0)
        self.msb_first_var = tk.BooleanVar(value=True)
        self.invert_polarity_var = tk.BooleanVar(value=False)
        self.pil_image = None
        self.tk_image = None
        self.filepath = None
        # --- Frames and Widgets setup ---
        top_frame = ttk.Frame(root, padding="10"); top_frame.pack(fill=tk.X)
        ttk.Button(top_frame, text="Select Bitmap File", command=self.select_file).pack(side=tk.LEFT, padx=(0,5))
        self.save_button = ttk.Button(top_frame, text="Save as PNG...", command=self.save_as_png, state=tk.DISABLED)
        self.save_button.pack(side=tk.LEFT, padx=5)
        ttk.Label(top_frame, text="File:").pack(side=tk.LEFT, padx=(10, 5))
        ttk.Label(top_frame, textvariable=self.current_file, relief=tk.SUNKEN, padding="2", anchor=tk.W).pack(side=tk.LEFT, fill=tk.X, expand=True)
        width_frame = ttk.Frame(root, padding="5 10"); width_frame.pack(fill=tk.X)
        ttk.Label(width_frame, text="Width:").pack(side=tk.LEFT, padx=5)
        self.width_entry = ttk.Entry(width_frame, textvariable=self.image_width, width=5, validate="key")
        vcmd = (self.root.register(self.validate_width_input), '%P')
        self.width_entry.config(validatecommand=vcmd); self.width_entry.pack(side=tk.LEFT, padx=5)
        self.width_entry.bind("<Return>", self.update_image_from_ui_event)
        self.width_entry.bind("<FocusOut>", self.update_image_from_ui_event)
        self.width_slider = ttk.Scale(width_frame, from_=1, to=MAX_SLIDER_WIDTH, orient=tk.HORIZONTAL, variable=self.image_width, command=self.slider_command_handler)
        self.width_slider.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=5)
        options_frame = ttk.Frame(root, padding="0 5 10 5"); options_frame.pack(fill=tk.X)
        ttk.Checkbutton(options_frame, text="MSB First", variable=self.msb_first_var, command=self.schedule_update).pack(side=tk.LEFT, padx=10)
        ttk.Checkbutton(options_frame, text="Invert Polarity", variable=self.invert_polarity_var, command=self.schedule_update).pack(side=tk.LEFT, padx=10)
        img_frame = ttk.Frame(root, relief=tk.GROOVE, borderwidth=1); img_frame.pack(padx=10, pady=10, fill=tk.BOTH, expand=True)
        self.canvas = tk.Canvas(img_frame, bg="gray")
        v_scrollbar = ttk.Scrollbar(img_frame, orient=tk.VERTICAL, command=self.canvas.yview)
        h_scrollbar = ttk.Scrollbar(img_frame, orient=tk.HORIZONTAL, command=self.canvas.xview)
        self.canvas.configure(yscrollcommand=v_scrollbar.set, xscrollcommand=h_scrollbar.set)
        v_scrollbar.grid(row=0, column=1, sticky='ns'); h_scrollbar.grid(row=1, column=0, sticky='ew')
        self.canvas.grid(row=0, column=0, sticky='nsew')
        img_frame.grid_rowconfigure(0, weight=1); img_frame.grid_columnconfigure(0, weight=1)
        self.canvas_text_id = self.canvas.create_text(10, 10, anchor='nw', text="Select a file...")
        self.canvas_image_id = None
        status_frame = ttk.Frame(root, padding="5", relief=tk.SUNKEN); status_frame.pack(fill=tk.X, side=tk.BOTTOM)
        self.status_label = ttk.Label(status_frame, text="Width: ? Height: ?")
        self.status_label.pack(side=tk.LEFT)

    # ... (validate_width_input, select_file, save_as_png remain the same) ...
    def validate_width_input(self, P):
        if P.isdigit(): return int(P) >= 1
        elif P == "": return True
        else: return False

    def select_file(self):
        filepath = filedialog.askopenfilename(
            initialdir=os.getcwd(), title="Select Printer Data File",
            filetypes=(("Bitmap/Binary", "*.bmp;*.bin"), ("All files", "*.*"))
        )
        if filepath:
            self.filepath = filepath
            self.current_file.set(os.path.basename(self.filepath))
            self.image_width.set(DEFAULT_WIDTH) # Start at default
            self.msb_first_var.set(True)
            self.invert_polarity_var.set(False)
            self.schedule_update() # Update display

    def save_as_png(self):
        if self.pil_image is None: messagebox.showerror("Error", "No image."); return
        if not self.filepath: messagebox.showerror("Error", "No source path."); return
        base = os.path.basename(self.filepath); name, _ = os.path.splitext(base)
        default_filename = name + "_processed.png" # Suggest different name
        save_path = filedialog.asksaveasfilename(
            title="Save as PNG", initialfile=default_filename, defaultextension=".png",
            filetypes=(("PNG", "*.png"), ("All", "*.*"))
        )
        if save_path:
            try: self.pil_image.save(save_path, "PNG"); self.status_label.config(text=f"Saved: {os.path.basename(save_path)}")
            except Exception as e: messagebox.showerror("Save Error", f"Failed: {e}"); self.status_label.config(text="Save failed.")


    def slider_command_handler(self, value_str):
        self.schedule_update()

    def update_image_from_ui_event(self, event=None):
        self.schedule_update()

    def schedule_update(self):
         if self._update_job is not None: self.root.after_cancel(self._update_job)
         self._update_job = self.root.after(100, self.update_image_from_ui)

    def update_image_from_ui(self):
        self._update_job = None
        if self.canvas_image_id: self.canvas.delete(self.canvas_image_id); self.canvas_image_id = None
        if self.canvas_text_id: self.canvas.delete(self.canvas_text_id); self.canvas_text_id = None
        self.canvas.configure(scrollregion=self.canvas.bbox('all'))

        if not self.filepath:
            self.canvas_text_id = self.canvas.create_text(10, 10, anchor='nw', text="Select a file...")
            self._reset_ui_state()
            return

        try:
            width = self.image_width.get()
            if width <= 0: raise ValueError("Width must be positive")
        except (tk.TclError, ValueError) as e:
             self.status_label.config(text=f"Invalid width value ({e}).")
             self.canvas_text_id = self.canvas.create_text(10, 10, anchor='nw', text="Invalid Width")
             self._reset_ui_state()
             return

        msb_first = self.msb_first_var.get()
        invert_polarity = self.invert_polarity_var.get()
        # Call display, which now handles the tuple with removed_bytes count
        self.display_image(self.filepath, width, msb_first, invert_polarity)

    def _reset_ui_state(self):
        self.tk_image = None
        self.pil_image = None
        self.save_button.config(state=tk.DISABLED)

    # Modified display_image to handle new return tuple and update status
    def display_image(self, filename, width, msb_first, invert_polarity):
        options_str = f"{'MSB' if msb_first else 'LSB'}{', INV' if invert_polarity else ''}"
        self.status_label.config(text=f"Processing W:{width} ({options_str})...")
        self.root.update_idletasks()

        # Load image - expecting potentially 4 values now
        result_tuple = load_and_process_bitmap(filename, width, msb_first, invert_polarity)
        pil_img, w, h, removed_bytes = None, 0, 0, 0 # Initialize defaults
        if result_tuple is not None and len(result_tuple) >= 3:
            pil_img = result_tuple[0]
            w = result_tuple[1]
            h = result_tuple[2]
            if len(result_tuple) > 3: # Check if removed_bytes was returned
                removed_bytes = result_tuple[3]

        self.pil_image = pil_img # Store reference FIRST

        status_extra = f" ({removed_bytes} bytes removed)" if removed_bytes > 0 else ""

        if self.pil_image:
            try:
                self.tk_image = ImageTk.PhotoImage(self.pil_image) # Convert
                if self.canvas_text_id: self.canvas.delete(self.canvas_text_id); self.canvas_text_id = None
                if self.canvas_image_id: self.canvas.delete(self.canvas_image_id); self.canvas_image_id = None
                self.canvas_image_id = self.canvas.create_image(0, 0, anchor='nw', image=self.tk_image)
                self.canvas.config(scrollregion=self.canvas.bbox(self.canvas_image_id))
                self.image_height.set(h)
                self.status_label.config(text=f"W:{w} H:{h} ({options_str}){status_extra}") # Add removed info
                self.save_button.config(state=tk.NORMAL)

            except Exception as e:
                 messagebox.showerror("Display Error", f"Tkinter image failed:\n{e}")
                 self._clear_canvas_display("Display Error")
                 self._reset_ui_state()
                 self.status_label.config(text=f"Display Error (W:{w}, H:{h}){status_extra}")
        else:
            # load_and_process_bitmap failed or returned None image
            self._clear_canvas_display("Load/Process Failed")
            self._reset_ui_state() # pil_image is already None
            self.status_label.config(text=f"Load/Process Failed.{status_extra}")

    # _clear_canvas_display remains the same
    def _clear_canvas_display(self, message=""):
        if self.canvas_image_id: self.canvas.delete(self.canvas_image_id); self.canvas_image_id = None
        if self.canvas_text_id: self.canvas.delete(self.canvas_text_id); self.canvas_text_id = None
        if message: self.canvas_text_id = self.canvas.create_text(10, 10, anchor='nw', text=message)
        self.canvas.config(scrollregion=self.canvas.bbox('all'))


# --- Main Execution ---
if __name__ == "__main__":
    root = tk.Tk()
    # Check if sequence removal is configured
    if SEQUENCE_TO_REMOVE1 is None:
        print("Warning: Sequence removal is disabled due to invalid hex string.")
    if SEQUENCE_TO_REMOVE2 is None:
        print("Warning: Sequence removal is disabled due to invalid hex string.")
    app = BitmapViewerApp(root)
    root.mainloop()