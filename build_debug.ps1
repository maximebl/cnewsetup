$projname = "cnewsetup"
$output_path = "$PSScriptRoot\bin\debug"

if(!(Test-Path $output_path))
{
    new-item $output_path -ItemType "directory"
}

Push-Location "$output_path"

$compiler = "$PSScriptRoot\bin\compiler\clang.exe"
$linker = "$PSScriptRoot\bin\compiler\lld-link.exe"


#pch
$pch_source = (Get-Item "$PSScriptRoot\source\cnewsetup.h" -ErrorAction SilentlyContinue) 
$pch = (Get-Item "$output_path\cnewsetup.h.pch" -ErrorAction SilentlyContinue)

if($pch_source.LastWriteTime -gt $pch.LastWriteTime)
{
    Write-Host $pch_source.Name "was modified, recompiling cnewsetup.h.pch" -ForegroundColor Yellow
    &$compiler -g -x c-header "$PSScriptRoot\source\cnewsetup.h" `
    -D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
    -D "CINTERFACE" `
    -o "$output_path\cnewsetup.h.pch"
}

# game_code
$gamecode_source_path = "$PSScriptRoot\source"
$gamecode_source_files = @((Get-Item "$PSScriptRoot\source\game_code.c"), (Get-Item "$PSScriptRoot\source\imgui_impl_dx12.c"), (Get-Item "$PSScriptRoot\source\imgui_impl_win32.c"))
$last_gamecode_compilation_output = (Get-Item "$output_path\game_code.dll" -ErrorAction SilentlyContinue)

foreach($file in $gamecode_source_files)
{
    $file_was_modified = ($file.LastWriteTime) -gt ($last_gamecode_compilation_output.LastWriteTime)
        
    if($last_gamecode_compilation_output.Exists)
    {
        if(!$file_was_modified)
        {
            continue
        }
        else
        {
            Write-Host $file.Name "was modified, recompiling game_code.dll" -ForegroundColor DarkGreen
        }
    }
    else
    {
        Write-Host "game_code.dll not found, compiling game_code.dll" -ForegroundColor DarkGreen
    }
            
    $pre_compilation = (Get-Date)
    &$compiler -std=c11 -g -c -Weverything -include "$PSScriptRoot\source\cnewsetup.h" `
    -D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
    -D "CINTERFACE" `
    "$PSScriptRoot\source\game_code.c" `
    "$PSScriptRoot\source\imgui_impl_dx12.c" `
    "$PSScriptRoot\source\imgui_impl_win32.c"

    $post_compilation = (Get-Date)
    $compile_time = New-TimeSpan –Start $pre_compilation –End $post_compilation
    $pre_linking = (Get-Date)

    Write-Host "Linking game_code.dll" -ForegroundColor DarkGreen
    &$linker -dll -debug -subsystem:windows -defaultlib:libcmt `
    -libpath:"$PSScriptRoot\bin\dependencies" `
    "$output_path\game_code.o" `
    "$output_path\imgui_impl_win32.o" `
    "$output_path\imgui_impl_dx12.o"

    $post_linking = (Get-Date)
    $link_time = New-TimeSpan –Start $pre_linking –End $post_linking
    Write-Host "Compilation took" $compile_time.TotalMilliseconds"ms" -ForegroundColor DarkGreen
    Write-Host "Linking took" $link_time.TotalMilliseconds"ms" -ForegroundColor DarkGreen
    Write-Host "Total" ($link_time.TotalMilliseconds + $compile_time.TotalMilliseconds)"ms" -ForegroundColor DarkGreen

    break
}

# executable
Write-Host "Compiling $projname.exe" -ForegroundColor Blue
&$compiler -std=c11 -g -c -Weverything -include "$PSScriptRoot\source\cnewsetup.h" `
-D "CIMGUI_DEFINE_ENUMS_AND_STRUCTS" `
-D "CINTERFACE" `
"$PSScriptRoot\source\main.c"

Write-Host "Linking $projname.exe" -ForegroundColor Blue
&$linker -defaultlib:libcmt -debug -out:"$projname.exe" -libpath:"$PSScriptRoot\bin\dependencies" `
"$output_path\main.o"

Pop-Location
