import urllib.request
import os
import argparse

MAX_WORDS_PER_LINE = 16  # This results in 32 bytes per line


def read_binary_from_file(file_path):
    with open(file_path, "rb") as file:
        return file.read()


def binary_to_c_array(input_source, output_file, array_name, endian_format="little"):
    offset = 0

    data = read_binary_from_file(input_source)

    # Find the last non-zero byte in the data
    last_non_zero_index = -1
    for i in range(len(data) - 1, -1, -1):
        if data[i] != 0:
            last_non_zero_index = i
            break

    # If the entire file is zero, then effectively no data
    if last_non_zero_index == -1:
        trimmed_data = b''
    else:
        trimmed_data = data[: last_non_zero_index + 1]

    # Ensure that the trimmed binary data has an even length (since we're handling words)
    if len(trimmed_data) % 2 != 0:
        raise ValueError("The binary file size (after trimming zeros) should be an even number of bytes for word processing.")

    # Prepare the output content
    content = f"const uint16_t {array_name}[] = {{\n"

    # Convert trimmed data to comma-separated hex values with MAX_WORDS_PER_LINE words per line
    for i in range(offset, len(trimmed_data) - offset, 2 * MAX_WORDS_PER_LINE):
        chunk = trimmed_data[i : i + 2 * MAX_WORDS_PER_LINE]

        if endian_format == "big":
            words = [chunk[j] + (chunk[j + 1] << 8) for j in range(0, len(chunk), 2)]
        else:  # little endian
            words = [(chunk[j] << 8) + chunk[j + 1] for j in range(0, len(chunk), 2)]

        content += "    " + ", ".join(f"0x{word:04X}" for word in words) + ",\n"

    # Remove the trailing comma and add closing brace
    content = content.rstrip(",\n") + "\n};\n"
    content += f"uint16_t {array_name}_length = sizeof({array_name}) / sizeof({array_name}[0]);\n\n"

    # Write to output .h file
    with open(output_file, "w") as f:
        f.write(content)

    print(f"{output_file} generated successfully!")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a C array from a binary file, trimming trailing zeros."
    )
    parser.add_argument(
        "--input",
        required=True,
        default="",
        help="Path to the input binary file.",
    )
    parser.add_argument(
        "--output",
        required=True,
        default="",
        help="Output .h (header) file to write the array.",
    )
    parser.add_argument(
        "--array_name",
        required=True,
        default="",
        help="Name of the array to be generated.",
    )
    parser.add_argument(
        "--endian_format",
        required=False,
        default="little",
        help="Endianness of the words in the output array ('little' or 'big').",
    )

    args = parser.parse_args()
    array_name = args.array_name
    output_file = args.output
    input_source = args.input
    endian_format = args.endian_format

    binary_to_c_array(input_source, output_file, array_name, endian_format)
