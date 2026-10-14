param([string]$Path,[string]$Out)
$b = [IO.File]::ReadAllBytes($Path)
function U16($o){ [BitConverter]::ToUInt16($b,$o) }
function U32($o){ [BitConverter]::ToUInt32($b,$o) }
function CStr($o){ $e=$o; while($b[$e] -ne 0){$e++}; [Text.Encoding]::ASCII.GetString($b,$o,$e-$o) }
$pe=U32 0x3C; $opt=$pe+24; $nsec=U16 ($pe+6); $secOff=$opt+(U16 ($pe+20))
$secs = for($i=0;$i -lt $nsec;$i++){ $o=$secOff+$i*40; @{VA=(U32 ($o+12)); VS=(U32 ($o+8)); RS=(U32 ($o+16)); RP=(U32 ($o+20))} }
function R2O($rva){ foreach($s in $secs){ if($rva -ge $s.VA -and $rva -lt $s.VA+[Math]::Max($s.VS,$s.RS)){ return $rva-$s.VA+$s.RP } } }
$ed = R2O (U32 ($opt+96))
"Export DLL name: " + (CStr (R2O (U32 ($ed+12))))
$nFuncs=U32 ($ed+20); $nNames=U32 ($ed+24)
$aF=R2O (U32 ($ed+28)); $aN=R2O (U32 ($ed+32)); $aO=R2O (U32 ($ed+36))
"Functions: $nFuncs  Named: $nNames"
$rows = for($i=0;$i -lt $nNames;$i++){
  $n = CStr (R2O (U32 ($aN+4*$i))); $ord = U16 ($aO+2*$i); $rva = U32 ($aF+4*$ord)
  "0x{0:X8}`t{1}" -f (0x400000+$rva), $n }
$rows | Set-Content -Encoding utf8 $Out
