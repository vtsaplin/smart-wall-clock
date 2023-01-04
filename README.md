# Smart Wall Clock

## Building and deploying

Install CLI tools:
```bash
brew install direnv
brew install --cask 1password/tap/1password-cli
bre install platformio
```

Set up the project:
```bash
cd ./<project_folder>
direnv allow .
op inject -i envrc -o .envrc 
```

Deploy:
```bash
pio run -t upload -e ota # OTA
pio run -t upload -e local # Local
```
