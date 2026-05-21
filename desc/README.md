# app.json Template Explanation

This file is a template for the `app.json` descriptor file used by each `.uf2` application. Before building your app, you should replace the placeholder values with the actual details for your application.

## Fields

- **uuid**  
  - A universally unique identifier for your application.  
  - **Format:** A 36-character UUID (e.g., `123e4567-e89b-12d3-a456-426614174000`).  
  - **Placeholder:** `<APP_UUID>`

- **name**  
  - The display name of your application in a human-readable format.
  - **Placeholder:** `<APP_NAME>`

- **description**  
  - A short description of what your application does.  
  - **Placeholder:** `<APP_DESCRIPTION>`

- **image**  
  - A URL to an image (such as an icon) representing your app. Optional.
  - **Recommended:** 256x256 PNG image or similar.  
  - **Placeholder:** `<APP_IMAGE_URL>`

- **tags**  
  - An array of strings used to categorize your app (for example, "Emulation", "ROM", "Utility").  
  - **Placeholder:** Replace with one or more tags (e.g., `"Emulation"`, `"ROM"`).

- **devices**  
  - An array listing the devices or hardware platforms supported by your app.  
  - **Placeholder:** Replace with the actual device names (e.g., `"Atari STE"`, `"Atari ST"`).

- **binary**  
  - The URL where the `.uf2` binary for your app is hosted.  
  - **Placeholder:** `<APP_BINARY_URL>`

- **md5**  
  - The MD5 checksum of the binary file, used for verification.  
  - **Placeholder:** `<BINARY_MD5_HASH>`

- **version**  
  - The version of your application (typically following semantic versioning, e.g., "1.0.0").  
  - **Placeholder:** `<APP_VERSION>`

## Example

After replacing the placeholders, your file might look like:

```json
{
  "uuid": "123e4567-e89b-12d3-a456-426614174000",
  "name": "My UF2 App",
  "description": "A simple demo application for the RP2040.",
  "image": "https://example.com/my-app-icon.png",
  "tags": [
    "Demo",
    "Utility"
  ],
  "devices": [
    "RP2040",
    "Custom Board"
  ],
  "binary": "https://example.com/my-app.uf2",
  "md5": "a1b2c3d4e5f67890123456789abcdef0",
  "version": "1.0.0"
}
```

## How to create the app.json file

1. Copy the contents of this file to a new file named `app.json` in the same `desc` folder.
2. Replace the placeholder values with the actual details for your application, avoiding the  `<APP_UUID>`, `<BINARY_MD5_HASH>`, and `<APP_VERSION>` placeholders; they will be filled by the `build.sh` script.
6. Save the file and commit it to your repository if necessary.


