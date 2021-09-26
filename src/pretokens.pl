#!/usr/bin/perl
#
use strict;
use warnings;

if ( ! -f "$ARGV[0]" ) {
  print "Usage: pretokens.pl file\n";
  print "    Opens file and adds MASK tokens and creates target output \n";
  exit(1);
}

open(my $file, "<", $ARGV[0]) || die "open $ARGV[0] failed: $!";
my $maskedfile=$ARGV[0];
$maskedfile =~ s/.raw.txt/.src.txt/;
my $tgtfile=$ARGV[0];
$tgtfile =~ s/.raw.txt/.tgt.txt/;
open(my $masked, ">", $maskedfile) || die "open $maskedfile failed: $!";
open(my $tgt, ">", $tgtfile) || die "open $tgtfile failed: $!";

while (<$file>) {
  s/ ..<S2SV>//g;
  s/\s+$//;
  my $maskout="";
  my $tgtout="";
  my $infill=0;
  foreach my $tok (split / +/) {
    $tgtout .= $tok." ";
    if ($infill && rand()<0.67) {
      # Continue infill stream with Poisson distribution 
    } else {
      $infill=0;
      # For 15% average effoct on tokens and 1/3 chance for each 
      # type (mask, delete, infill), any token has 5%+5%+(5%/3) = 11.67% 
      # chance of starting a modification
      if (rand()<0.115) {
        if (rand() < (5/11.5)) {
          # Token deletion, do not add to masked function
        } elsif (rand() < (5/6.5)) {
          # Token masking
          $maskout .= "<S2SV_MASK> ";
        } else {
          # Token infilling
          $maskout .= "<S2SV_MASK> ";
          $infill=1;
        }
      } else {
        $maskout .= $tok." ";
      }
    }
  }
  $maskout =~ s/ +$//;
  $tgtout =~ s/ +$//;
  
  print $masked $maskout."\n";
  print $tgt  $tgtout."\n";
}

close $masked;
close $tgt;
