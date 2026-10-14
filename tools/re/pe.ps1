param([string]$Path)
$b = [IO.File]::ReadAllBytes($Path)
function U16($o){ [BitConverter]::ToUInt16($b,$o) }
function U32($o){ [BitConverter]::ToUInt32($b,$o) }
function CStr($o){ $e=$o; while($b[$e] -ne 0){$e++}; [Text.Encoding]::ASCII.GetString($b,$o,$e-$o) }

$pe = U32 0x3C
$machine = U16 ($pe+4); $nsec = U16 ($pe+6); $ts = U32 ($pe+8)
$optSize = U16 ($pe+20); $chars = U16 ($pe+22)
$opt = $pe+24; $magic = U16 $opt
"PE offset      : 0x{0:X}" -f $pe
"Machine        : 0x{0:X4} ({1})" -f $machine, $(if($machine -eq 0x14c){"i386 / 32-bit x86"}else{"other"})
"Opt magic      : 0x{0:X} ({1})" -f $magic, $(if($magic -eq 0x10b){"PE32"}else{"PE32+"})
"TimeDateStamp  : 0x{0:X8} = {1:u}" -f $ts, ([DateTime]'1970-01-01').AddSeconds($ts)
"Characteristics: 0x{0:X4} (LARGE_ADDRESS_AWARE={1})" -f $chars, [bool]($chars -band 0x20)
"Linker         : {0}.{1}" -f $b[$opt+2], $b[$opt+3]
$entry = U32 ($opt+16); $imageBase = U32 ($opt+28)
"EntryPoint RVA : 0x{0:X}  ImageBase: 0x{1:X}" -f $entry, $imageBase
"Subsystem      : {0}  OSVer {1}.{2}" -f (U16 ($opt+68)), (U16 ($opt+40)), (U16 ($opt+42))
"DllCharacter.  : 0x{0:X4} (DYNAMIC_BASE={1}, NX={2})" -f (U16 ($opt+70)), [bool]((U16 ($opt+70)) -band 0x40), [bool]((U16 ($opt+70)) -band 0x100)
$dd = $opt + 96
$names = "Export","Import","Resource","Exception","Security","Reloc","Debug","Arch","GlobalPtr","TLS","LoadConfig","BoundImport","IAT","DelayImport","CLR","Rsvd"
$dirs=@{}
for($i=0;$i -lt 16;$i++){ $dirs[$names[$i]] = @((U32 ($dd+$i*8)), (U32 ($dd+$i*8+4))) }

$secOff = $opt + $optSize
$secs = for($i=0;$i -lt $nsec;$i++){
  $o=$secOff+$i*40
  [pscustomobject]@{ Name=([Text.Encoding]::ASCII.GetString($b,$o,8)).TrimEnd([char]0); VSize=(U32 ($o+8)); VA=(U32 ($o+12)); RawSize=(U32 ($o+16)); RawPtr=(U32 ($o+20)); Chars=('0x{0:X8}' -f (U32 ($o+36))) }
}
"`n== Sections =="
$secs | Format-Table Name,@{n='VA';e={'0x{0:X}' -f $_.VA}},VSize,RawSize,@{n='RawPtr';e={'0x{0:X}' -f $_.RawPtr}},Chars -AutoSize | Out-String
function R2O($rva){ foreach($s in $secs){ if($rva -ge $s.VA -and $rva -lt $s.VA+[Math]::Max($s.VSize,$s.RawSize)){ return $rva-$s.VA+$s.RawPtr } }; return $null }

"== Data dirs =="
foreach($n in $names){ if($dirs[$n][0]){ "{0,-12} RVA 0x{1:X}  size {2}" -f $n,$dirs[$n][0],$dirs[$n][1] } }

"`n== Imports =="
$io = R2O $dirs["Import"][0]
while($true){
  $oft=U32 $io; $nameRva=U32 ($io+12); $ft=U32 ($io+16)
  if($nameRva -eq 0){break}
  $dll = CStr (R2O $nameRva)
  $t = R2O $(if($oft){$oft}else{$ft})
  $fns=@()
  while($true){ $v=U32 $t; if($v -eq 0){break}
    if($v -band 0x80000000){ $fns += "#"+($v -band 0xFFFF) } else { $fns += CStr ((R2O $v)+2) }
    $t+=4 }
  "[{0}] ({1})" -f $dll, $fns.Count
  "    " + ($fns -join ", ")
  $io+=20
}
if($dirs["DelayImport"][0]){
 "`n== Delay imports =="
 $io = R2O $dirs["DelayImport"][0]
 while($true){ $attr=U32 $io; $nr=U32 ($io+4); if($nr -eq 0){break}
   if(-not ($attr -band 1)){ $nr -= $imageBase }
   "[{0}]" -f (CStr (R2O $nr)); $io+=32 }
}
