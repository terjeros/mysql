# -*- cperl -*-
# Copyright (C) 2004-2006 MySQL AB
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# This is a library file used by the Perl version of mysql-test-run,
# and is part of the translation of the Bourne shell script with the
# same name.

package mtr_report;
use strict;

use base qw(Exporter);
our @EXPORT= qw(report_option mtr_print_line mtr_print_thick_line
		mtr_print_header mtr_report mtr_report_stats
		mtr_warning mtr_error mtr_debug mtr_verbose
		mtr_verbose_restart mtr_report_test_passed
		mtr_report_test_skipped mtr_print
		mtr_report_test);

use mtr_match;
require "mtr_io.pl";

my $tot_real_time= 0;

our $timestamp= 0;
our $timediff= 0;
our $name;
our $verbose;
our $verbose_restart= 0;
our $timer= 1;

sub report_option {
  my ($opt, $value)= @_;

  # Convert - to _ in option name
  $opt =~ s/-/_/g;
  no strict 'refs';
  ${$opt}= $value;

  #print $name, " setting $opt to ", (defined $value? $value : "undef") ,"\n";
}

sub _name {
  return $name ? $name." " : undef;
}

sub _mtr_report_test_name ($) {
  my $tinfo= shift;
  my $tname= $tinfo->{name};

  return unless defined $verbose;

  # Add combination name if any
  $tname.= " '$tinfo->{combination}'"
    if defined $tinfo->{combination};

  print _name(), _timestamp();
  printf "%-40s ", $tname;
}


sub mtr_report_test_skipped ($) {
  my ($tinfo)= @_;
  $tinfo->{'result'}= 'MTR_RES_SKIPPED';

  mtr_report_test($tinfo);
}


sub mtr_report_test_passed ($) {
  my ($tinfo)= @_;

  # Save the timer value
  my $timer_str=  "";
  if ( $timer and -f "$::opt_vardir/log/timer" )
  {
    $timer_str= mtr_fromfile("$::opt_vardir/log/timer");
    $tinfo->{timer}= $timer_str;
  }

  # Set as passed unless already set
  if ( not defined $tinfo->{'result'} ){
    $tinfo->{'result'}= 'MTR_RES_PASSED';
  }

  mtr_report_test($tinfo);
}


sub mtr_report_test ($) {
  my ($tinfo)= @_;
  _mtr_report_test_name($tinfo);

  my $comment=  $tinfo->{'comment'};
  my $logfile=  $tinfo->{'logfile'};
  my $warnings= $tinfo->{'warnings'};
  my $result=   $tinfo->{'result'};

  if ($result eq 'MTR_RES_FAILED'){

    if ( $warnings )
    {
      mtr_report("[ fail ]  Found warnings in server log file!");
      mtr_report($warnings);
      return;
    }
    my $timeout= $tinfo->{'timeout'};
    if ( $timeout )
    {
      mtr_report("[ fail ]  timeout after $timeout minutes");
      mtr_report("\n$tinfo->{'comment'}");
      return;
    }
    else
    {
      mtr_report("[ fail ]");
    }

    if ( $logfile )
    {
      # Test failure was detected by test tool and its report
      # about what failed has been saved to file. Display the report.
      mtr_report("\n$logfile\n");
    }
    if ( $comment )
    {
      # The test failure has been detected by mysql-test-run.pl
      # when starting the servers or due to other error, the reason for
      # failing the test is saved in "comment"
      mtr_report("\n$comment\n");
    }

    if ( !$logfile and !$comment )
    {
      # Neither this script or the test tool has recorded info
      # about why the test has failed. Should be debugged.
      mtr_report("\nUnknown result, neither 'comment' or 'logfile' set");
    }
  }
  elsif ($result eq 'MTR_RES_SKIPPED')
  {
    if ( $tinfo->{'disable'} )
    {
      mtr_report("[ disabled ]  $comment");
    }
    elsif ( $comment )
    {
      if ( $tinfo->{skip_detected_by_test} )
      {
	mtr_report("[ skip ]. $comment");
      }
      else
      {
	mtr_report("[ skip ]  $comment");
      }
    }
    else
    {
      mtr_report("[ skip ]");
    }
  }
  elsif ($result eq 'MTR_RES_PASSED')
  {
    my $timer_str= $tinfo->{timer} || "";
    $tot_real_time += ($timer_str/1000);
    mtr_report("[ pass ] ", sprintf("%5s", $timer_str));

    # Show any problems check-testcase found
    if ( defined $tinfo->{'check'} )
    {
      mtr_report($tinfo->{'check'});
    }
  }
}


sub mtr_report_stats ($) {
  my $tests= shift;

  # ----------------------------------------------------------------------
  # Find out how we where doing
  # ----------------------------------------------------------------------

  my $tot_skiped= 0;
  my $tot_passed= 0;
  my $tot_failed= 0;
  my $tot_tests=  0;
  my $tot_restarts= 0;
  my $found_problems= 0;

  foreach my $tinfo (@$tests)
  {
    if ( $tinfo->{failures} )
    {
      # Test has failed at least one time
      $tot_tests++;
      $tot_failed++;
    }
    elsif ( $tinfo->{'result'} eq 'MTR_RES_SKIPPED' )
    {
      # Test was skipped
      $tot_skiped++;
    }
    elsif ( $tinfo->{'result'} eq 'MTR_RES_PASSED' )
    {
      # Test passed
      $tot_tests++;
      $tot_passed++;
    }

    if ( $tinfo->{'restarted'} )
    {
      # Servers was restarted
      $tot_restarts++;
    }

    # Look for warnings produced by mysqltest
    my $base_file= mtr_match_extension($tinfo->{'result_file'},
				       "result"); # Trim extension
    my $warning_file= "$base_file.warnings";
    if ( -f $warning_file )
    {
      $found_problems= 1;
      mtr_warning("Check myqltest warnings in '$warning_file'");
    }
  }

  # ----------------------------------------------------------------------
  # Print out a summary report to screen
  # ----------------------------------------------------------------------
  print "The servers were restarted $tot_restarts times\n";

  if ( $timer )
  {
    use English;

    mtr_report("Spent", sprintf("%.3f", $tot_real_time),"of",
	       time - $BASETIME, "seconds executing testcases");
  }


  my $warnlog= "$::opt_vardir/log/warnings";
  if ( -f $warnlog )
  {
    # Save and report if there was any fatal warnings/errors in err logs

    my $warnlog= "$::opt_vardir/log/warnings";

    unless ( open(WARN, ">$warnlog") )
    {
      mtr_warning("can't write to the file \"$warnlog\": $!");
    }
    else
    {
      # We report different types of problems in order
      foreach my $pattern ( "^Warning:",
			    "\\[Warning\\]",
			    "\\[ERROR\\]",
			    "^Error:", "^==.* at 0x",
			    "InnoDB: Warning",
			    "InnoDB: Error",
			    "^safe_mutex:",
			    "missing DBUG_RETURN",
			    "mysqld: Warning",
			    "allocated at line",
			    "Attempting backtrace", "Assertion .* failed" )
      {
        foreach my $errlog ( sort glob("$::opt_vardir/log/*.err") )
        {
	  my $testname= "";
          unless ( open(ERR, $errlog) )
          {
            mtr_warning("can't read $errlog");
            next;
          }
          my $leak_reports_expected= undef;
          while ( <ERR> )
          {
            # There is a test case that purposely provokes a
            # SAFEMALLOC leak report, even though there is no actual
            # leak. We need to detect this, and ignore the warning in
            # that case.
            if (/Begin safemalloc memory dump:/) {
              $leak_reports_expected= 1;
            } elsif (/End safemalloc memory dump./) {
              $leak_reports_expected= undef;
            }

            # Skip some non fatal warnings from the log files
            if (
		/\"SELECT UNIX_TIMESTAMP\(\)\" failed on master/ or
		/Aborted connection/ or
		/Client requested master to start replication from impossible position/ or
		/Could not find first log file name in binary log/ or
		/Enabling keys got errno/ or
		/Error reading master configuration/ or
		/Error reading packet/ or
		/Event Scheduler/ or
		/Failed to open log/ or
		/Failed to open the existing master info file/ or
		/Forcing shutdown of [0-9]* plugins/ or
                /Can't open shared library .*\bha_example\b/ or
                /Couldn't load plugin .*\bha_example\b/ or

		# Due to timing issues, it might be that this warning
		# is printed when the server shuts down and the
		# computer is loaded.
		/Forcing close of thread \d+  user: '.*?'/ or

		/Got error [0-9]* when reading table/ or
		/Incorrect definition of table/ or
		/Incorrect information in file/ or
		/InnoDB: Warning: we did not need to do crash recovery/ or
		/Invalid \(old\?\) table or database name/ or
		/Lock wait timeout exceeded/ or
		/Log entry on master is longer than max_allowed_packet/ or
                /unknown option '--loose-/ or
                /unknown variable 'loose-/ or
		/You have forced lower_case_table_names to 0 through a command-line option/ or
		/Setting lower_case_table_names=2/ or
		/NDB Binlog:/ or
		/NDB: failed to setup table/ or
		/NDB: only row based binary logging/ or
		/Neither --relay-log nor --relay-log-index were used/ or
		/Query partially completed/ or
		/Slave I.O thread aborted while waiting for relay log/ or
		/Slave SQL thread is stopped because UNTIL condition/ or
		/Slave SQL thread retried transaction/ or
		/Slave \(additional info\)/ or
		/Slave: .*Duplicate column name/ or
		/Slave: .*master may suffer from/ or
		/Slave: According to the master's version/ or
		/Slave: Column [0-9]* type mismatch/ or
                /Slave: Can't DROP 'c7'; check that column.key exists Error_code: 1091/ or
                /Slave: Unknown column 'c7' in 't15' Error_code: 1054/ or
                /Slave: Key column 'c6' doesn't exist in table Error_code: 1072/ or
		/Slave: Error .* doesn't exist/ or
		/Slave: Error .*Deadlock found/ or
		/Slave: Error .*Unknown table/ or
		/Slave: Error in Write_rows event: / or
		/Slave: Field .* of table .* has no default value/ or
                /Slave: Field .* doesn't have a default value/ or
		/Slave: Query caused different errors on master and slave/ or
		/Slave: Table .* doesn't exist/ or
		/Slave: Table width mismatch/ or
		/Slave: The incident LOST_EVENTS occured on the master/ or
		/Slave: Unknown error.* 1105/ or
		/Slave: Can't drop database.* database doesn't exist/ or
                /Slave SQL:.*(?:Error_code: \d+|Query:.*)/ or
		
		# backup_errors test is supposed to trigger lots of backup related errors
		($testname eq 'main.backup_errors') and
		(
		  /Backup:/ or /Restore:/ or /Can't open the online backup progress tables/
		) or

		# backup_backupdir test is supposed to trigger backup related errors
		($testname eq 'main.backup_backupdir') and
		(
		  /Backup:/ or /Can't write to backup location/
		) or
                
		# backup_concurrent performs a backup that should fail
		($testname eq 'main.backup_concurrent') and
		(
		  /Can't execute this command because another BACKUP\/RESTORE operation is in progress/
		) or
                
		# The tablespace test triggers error below on purpose
		($testname eq 'main.backup_tablespace') and
		(
		  /Restore: Tablespace .* needed by tables being restored has changed on the server/
		) or
		
		# The views test triggers errors below on purpose
		($testname eq 'main.backup_views') and
		(
		  /Backup: Failed to add view/ or
		  /Backup: Failed to obtain meta-data for view/ or
		  /Restore: Could not restore view/
		) or
 	 
		# ignore warning generated when backup engine selection algorithm is tested
		($testname eq 'main.backup_no_be') and /Backup: Cannot create backup engine/ or
		# ignore warnings generated when backup privilege is tested
		($testname eq 'main.backup_security') and /(Backup|Restore): Access denied; you need the SUPER/ or
		
                ($testname eq 'main.backup_myisam1') and
                (/Backup: Can't initialize MyISAM backup driver/) or
		/Sort aborted/ or
		/Time-out in NDB/ or
		/One can only use the --user.*root/ or
		/Table:.* on (delete|rename)/ or
		/You have an error in your SQL syntax/ or
		/deprecated/ or
		/description of time zone/ or
		/equal MySQL server ids/ or
		/error .*connecting to master/ or
		/error reading log entry/ or
		/lower_case_table_names is set/ or
		/skip-name-resolve mode/ or
		/slave SQL thread aborted/ or
		/Slave: .*Duplicate entry/ or
		# Special case for Bug #26402 in show_check.test
		# Question marks are not valid file name parts
		# on Windows platforms. Ignore this error message. 
		/\QCan't find file: '.\test\????????.frm'\E/ or
		# Special case, made as specific as possible, for:
		# Bug #28436: Incorrect position in SHOW BINLOG EVENTS causes
		#             server coredump
		/\QError in Log_event::read_log_event(): 'Sanity check failed', data_len: 258, event_type: 49\E/ or
                /Statement is not safe to log in statement format/ or

                # test case for Bug#bug29807 copies a stray frm into database
                /InnoDB: Error: table `test`.`bug29807` does not exist in the InnoDB internal/ or
                /Cannot find or open table test\/bug29807 from/ or

                # innodb foreign key tests that fail in ALTER or RENAME produce this
                /InnoDB: Error: in ALTER TABLE `test`.`t[12]`/ or
                /InnoDB: Error: in RENAME TABLE table `test`.`t1`/ or
                /InnoDB: Error: table `test`.`t[12]` does not exist in the InnoDB internal/ or

                # Test case for Bug#14233 produces the following warnings:
                /Stored routine 'test'.'bug14233_1': invalid value in column mysql.proc/ or
                /Stored routine 'test'.'bug14233_2': invalid value in column mysql.proc/ or
                /Stored routine 'test'.'bug14233_3': invalid value in column mysql.proc/ or

                # BUG#29839 - lowercase_table3.test: Cannot find table test/T1
                #             from the internal data dictiona
                /Cannot find table test\/BUG29839 from the internal data dictionary/ or
                # BUG#32080 - Excessive warnings on Solaris: setrlimit could not
                #             change the size of core files
                /setrlimit could not change the size of core files to 'infinity'/ or

                # rpl_ndb_basic expects this error
                /Slave: Got error 146 during COMMIT Error_code: 1180/ or

		# rpl_extrColmaster_*.test, the slave thread produces warnings
		# when it get updates to a table that has more columns on the
		# master
		/Slave: Unknown column 'c7' in 't15' Error_code: 1054/ or
		/Slave: Can't DROP 'c7'.* 1091/ or
		/Slave: Key column 'c6'.* 1072/ or

                # BUG#32080 - Excessive warnings on Solaris: setrlimit could not
                #             change the size of core files
                /setrlimit could not change the size of core files to 'infinity'/ or
                # rpl_idempotency.test produces warnings for the slave.
                ($testname eq 'rpl.rpl_idempotency' and
                 (/Slave: Can\'t find record in \'t1\' Error_code: 1032/ or
                   /Slave: Cannot add or update a child row: a foreign key constraint fails .* Error_code: 1452/
                 )) or
 
                # These tests does "kill" on queries, causing sporadic errors when writing to logs
                (($testname eq 'rpl.rpl_skip_error' or
                  $testname eq 'rpl.rpl_err_ignoredtable' or
                  $testname eq 'binlog.binlog_killed_simulate' or
                  $testname eq 'binlog.binlog_killed') and
                 (/Failed to write to mysql\.\w+_log/
                 )) or

		# rpl_bug33931 has deliberate failures
		($testname eq 'rpl.rpl_bug33931' and
		 (/Failed during slave.*thread initialization/
		  )) or

                # rpl_temporary has an error on slave that can be ignored
                ($testname eq 'rpl.rpl_temporary' and
                 (/Slave: Can\'t find record in \'user\' Error_code: 1032/
                 )) or
                # Test case for Bug#31590 produces the following error:
                /Out of sort memory; increase server sort buffer size/ or
                # maria-recovery.test has warning about missing log file
                /File '.*maria_log.000.*' not found \(Errcode: 2\)/ or
                # and about marked-corrupted table
                /Table '.\/mysqltest\/t_corrupted1' is crashed, skipping it. Please repair it with maria_chk -r/ or
                # maria-recover.test corrupts tables on purpose
                /Checking table:   '.\/mysqltest\/t_corrupted2'/ or
                /Recovering table: '.\/mysqltest\/t_corrupted2'/ or
                /Table '.\/mysqltest\/t_corrupted2' is marked as crashed and should be repaired/ or
                /Incorrect key file for table '.\/mysqltest\/t_corrupted2.MAI'; try to repair it/ or
                # Bug#35161, test of auto repair --myisam-recover
                /able.*_will_crash/ or
                /Got an error from unknown thread, ha_myisam.cc:/ or

                # lowercase_table3 using case sensitive option on
                # case insensitive filesystem (InnoDB error).
                /Cannot find or open table test\/BUG29839 from/ or

                # When trying to set lower_case_table_names = 2
                # on a case sensitive file system. Bug#37402.
                /lower_case_table_names was set to 2, even though your the file system '.*' is case sensitive.  Now setting lower_case_table_names to 0 to avoid future problems./ or

                # Bug#20129 test of crashed tables
                /Got an error from thread_id=.*, ha_myisam.cc:/ or
                /MySQL thread id .*, query id .* Checking table/
              )
            {
              next;                       # Skip these lines
            }
	    if ( /CURRENT_TEST: (.*)/ )
	    {
	      $testname= $1;
	    }
            if ( /$pattern/ )
            {
              if ($leak_reports_expected) {
                next;
              }
              $found_problems= 1;
              print WARN basename($errlog) . ": $testname: $_";
            }
          }
        }
      }

      if ( $::opt_check_testcases )
      {
        # Look for warnings produced by mysqltest in testname.warnings
        foreach my $test_warning_file
	  ( glob("$::glob_mysql_test_dir/r/*.warnings") )
        {
          $found_problems= 1;
	  print WARN "Check myqltest warnings in $test_warning_file\n";
        }
      }

      if ( $found_problems )
      {
	mtr_warning("Got errors/warnings while running tests, please examine",
                    "'$warnlog' for details.");
      }
    }
  }

  print "\n";

  # Print a list of check_testcases that failed(if any)
  if ( $::opt_check_testcases )
  {
    my %check_testcases;

    foreach my $tinfo (@$tests)
    {
      if ( defined $tinfo->{'check_testcase_failed'} )
      {
	$check_testcases{$tinfo->{'name'}}= 1;
      }
    }

    if ( keys %check_testcases )
    {
      print "Check of testcase failed for: ";
      print join(" ", keys %check_testcases);
      print "\n\n";
    }
  }

  # Print a list of testcases that failed
  if ( $tot_failed != 0 )
  {

    # Print each failed test, again
    #foreach my $test ( @$tests ){
    #  if ( $test->{failures} ) {
    #    mtr_report_test($test);
    #  }
    #}

    my $ratio=  $tot_passed * 100 / $tot_tests;
    print "Failed $tot_failed/$tot_tests tests, ";
    printf("%.2f", $ratio);
    print "\% were successful.\n\n";

    # Print the list of test that failed in a format
    # that can be copy pasted to rerun only failing tests
    print "Failing test(s):";

    my %seen= ();
    foreach my $tinfo (@$tests)
    {
      my $tname= $tinfo->{'name'};
      if ( $tinfo->{failures} and ! $seen{$tname})
      {
        print " $tname";
	$seen{$tname}= 1;
      }
    }
    print "\n\n";

    # Print info about reporting the error
    print
      "The log files in var/log may give you some hint of what went wrong.\n\n",
      "If you want to report this error, please read first ",
      "the documentation\n",
      "at http://dev.mysql.com/doc/mysql/en/mysql-test-suite.html\n\n";

   }
  else
  {
    print "All $tot_tests tests were successful.\n\n";
  }

  if ( $tot_failed != 0 || $found_problems)
  {
    mtr_error("there were failing test cases");
  }
}


##############################################################################
#
#  Text formatting
#
##############################################################################

sub mtr_print_line () {
  print '-' x 60, "\n";
}


sub mtr_print_thick_line {
  my $char= shift || '=';
  print $char x 60, "\n";
}


sub mtr_print_header () {
  print "\n";
  printf "TEST";
  print " " x 38;
  print "RESULT   ";
  print "TIME (ms)" if $timer;
  print "\n";
  mtr_print_line();
  print "\n";
}


##############################################################################
#
#  Log and reporting functions
#
##############################################################################

use Time::localtime;

use Time::HiRes qw(gettimeofday);

my $t0= gettimeofday();

sub _timestamp {
  return "" unless $timestamp;

  my $diff;
  if ($timediff){
    my $t1= gettimeofday();
    my $elapsed= $t1 - $t0;

    $diff= sprintf(" +%02.3f", $elapsed);

    # Save current time for next lap
    $t0= $t1;

  }

  my $tm= localtime();
  return sprintf("%02d%02d%02d %2d:%02d:%02d%s ",
		 $tm->year % 100, $tm->mon+1, $tm->mday,
		 $tm->hour, $tm->min, $tm->sec, $diff);
}

# Always print message to screen
sub mtr_print (@) {
  print _name(), join(" ", @_), "\n";
}


# Print message to screen if verbose is defined
sub mtr_report (@) {
  if (defined $verbose)
  {
    print _name(), join(" ", @_), "\n";
  }
}


# Print warning to screen
sub mtr_warning (@) {
  print STDERR _name(), _timestamp(),
    "mysql-test-run: WARNING: ", join(" ", @_), "\n";
}


# Print error to screen and then exit
sub mtr_error (@) {
  print STDERR _name(), _timestamp(),
    "mysql-test-run: *** ERROR: ", join(" ", @_), "\n";
  exit(1);
}


sub mtr_debug (@) {
  if ( $verbose > 2 )
  {
    print STDERR _name(),
      _timestamp(), "####: ", join(" ", @_), "\n";
  }
}


sub mtr_verbose (@) {
  if ( $verbose )
  {
    print STDERR _name(), _timestamp(),
      "> ",join(" ", @_),"\n";
  }
}


sub mtr_verbose_restart (@) {
  my ($server, @args)= @_;
  my $proc= $server->{proc};
  if ( $verbose_restart )
  {
    print STDERR _name(),_timestamp(),
      "> Restart $proc - ",join(" ", @args),"\n";
  }
}


1;
