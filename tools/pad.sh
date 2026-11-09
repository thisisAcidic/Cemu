#!/system/bin/sh
# Thor gamepad injection: pad.sh press <code> | hold <code> <sec> | stick <l|r> <x> <y> <sec> | look <sec>
D=/dev/input/event9
syn() { sendevent $D 0 0 0; }
case "$1" in
  press) sendevent $D 1 $2 1; syn; sleep 0.15; sendevent $D 1 $2 0; syn ;;
  hold) sendevent $D 1 $2 1; syn; sleep $3; sendevent $D 1 $2 0; syn ;;
  stick)
    if [ "$2" = r ]; then AX=2; AY=5; else AX=0; AY=1; fi
    sendevent $D 3 $AX $3; sendevent $D 3 $AY $4; syn; sleep $5
    sendevent $D 3 $AX 0; sendevent $D 3 $AY 0; syn ;;
  look)
    # sweep the camera around with the right stick for about $2 seconds
    end=$(( $(date +%s) + $2 ))
    while [ $(date +%s) -lt $end ]; do
      for v in "26000 0" "0 -16000" "-26000 0" "0 16000" "20000 12000" "-20000 -12000"; do
        set -- $v
        sendevent $D 3 2 $1; sendevent $D 3 5 $2; syn; sleep 1.5
      done
    done
    sendevent $D 3 2 0; sendevent $D 3 5 0; syn ;;
esac
