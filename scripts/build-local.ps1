param([string]$Build = 'build', [string]$Type = 'Release', [string]$Sanitizer = '')
python scripts/build.py --build $Build --type $Type "--sanitizer=$Sanitizer"
exit $LASTEXITCODE
