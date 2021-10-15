#!/usr/bin/perl
#
use strict;
use warnings;

if ( ! -f "$ARGV[0]" || ($#ARGV>0 && ! -d $ARGV[1]) || ($#ARGV > 1)) {
  print "Usage: prespecial.pl file [dir]\n";
  print "    Opens file and adds MASK tokens and creates target output \n";
  print "    Optional dir insures no duplicate masked functions with that dir\n";
  exit(1);
}

open(my $file, "<", $ARGV[0]) || die "open $ARGV[0] failed: $!";
my $maskedfile=$ARGV[0];
$maskedfile =~ s/.raw.txt/.src.txt/;
my $tgtfile=$ARGV[0];
$tgtfile =~ s/.raw.txt/.tgt.txt/;
open(my $masked, ">", $maskedfile) || die "open $maskedfile failed: $!";
open(my $tgt, ">", $tgtfile) || die "open $tgtfile failed: $!";
my %maskedfunc;
if ($#ARGV == 1) {
  open(my $other, "<", "$ARGV[1]/$maskedfile") || die "open $ARGV[1]/$maskedfile failed: $!";
  while (<$other>) {
    $maskedfunc{$_}=1;
  }
  close $other;
}


while (<$file>) {
  s/\s+$//;
  my $maskout="";
  my $tgtout="";
  my $masking=0;
  my $deleting=0;
  my $infilling=0;
  # p3 is 3 context tokens back from current
  # When p3 is null, new changes can't start
  # When p1 is null, change is still active
  my $p3 = "<S2SV_null>";
  my $p2 = "<S2SV_null>";
  my $p1 = "<S2SV_null>";
  my $first=1;
  my $line="";
  foreach my $tok (split / +/) {
    if ($tok =~ /\/\/<S2SV>/) {
      if ($first && $tgtout) {
         $maskout .= "<S2SV_StartBug> ".$line."<S2SV_EndBug> ";
         $first=0;
      } else {
         $maskout .= $line;
      }
      $line="";
    } elsif (($p3 eq "") && ($p1 ne "")) {
      # Completing a change
      if (!$deleting) {
        $tgtout .= $tok." ";
      }
      $line.=$tok." ";
      $p3=$p2;
      $p2=$p1;
      $p1=$tok;
      if ($p3 ne "") {
        $masking=0;
        $deleting=0;
        $infilling=0;
      }
    } elsif ($p1 eq "") {
      # Change active
      if (rand()<0.33) {
        # Poisson distribution with lambda=3 for all changes
        # Change is ending
        $p1=$tok;
        $line.=$tok." ";
        if (!$deleting) {
          $tgtout .= "<S2SV_ModEnd> ".$tok." ";
        }
      } elsif ($masking) {
        $line.="<S2SV_MASK> ";
        $tgtout .= $tok." ";
      } else { # deleting or infilling
        $tgtout .= $tok." ";
      }
    } elsif (rand() < 0.05) {
      # Start a modification
      if (rand() < 0.33) {
        # Start masking
        $masking=1;
        $tgtout .= "<S2SV_ModStart> ".$p3." ".$p2." ".$p1." ".$tok." ";
        $line .= "<S2SV_MASK> ";
        $p1="";
      } elsif (rand() < 0.5) {
        # Start deleting
        $deleting=1;
        $tgtout .= "<S2SV_ModStart> ".$p3." ".$p2." ".$p1." ".$tok." ";
        $p1="";
      } else {
        # Start infilling
        $infilling=1;
        $tgtout .= "<S2SV_ModStart> ".$p3." ".$p2." ".$p1." ";
        $line .= "<S2SV_MASK> ";
        if (rand()<0.33) {
          # 0-length infill (delete mask token)
          $line .= $tok." ";
          $p1=$tok;
        } else {
          $tgtout .= $tok." ";
          $p1="";
        }
      }
      $p2="";
      $p3="";
    } else {
      $p3=$p2;
      $p2=$p1;
      $p1=$tok;
      $line .= $tok." ";
    }
  }
  if ($p1 eq "") { 
    if (!$deleting) {
      $tgtout .= "<S2SV_ModEnd> <S2SV_null> <S2SV_null> <S2SV_null> ";
    }
  } elsif ($p2 eq "") {
    if (!$deleting) {
      $tgtout .= "<S2SV_null> <S2SV_null> ";
    }
  } elsif ($p3 eq "") {
    if (!$deleting) {
      $tgtout .= "<S2SV_null> ";
    }
  }
  # Don't print out empty src or tgt
  $maskout =~ s/ +$/\n/ || next;
  $tgtout =~ s/ +$/\n/ || next;
  
  if (! $maskedfunc{$maskout}) {
    $maskedfunc{$maskout}=1;
    print $masked $maskout;
    print $tgt    $tgtout;
  }
}

close $masked;
close $tgt;
