#
# Include file for testing ndb$epoch conflict function
#
#

create table `test`.`t1$EX`
  (server_id int unsigned,
   master_server_id int unsigned,
   master_epoch bigint unsigned,
   count int unsigned,
   a int not null,
   d int,
   primary key(server_id, master_server_id, master_epoch, count)) engine ndb;


--echo Create table
create table test.t1(a int primary key, b varchar(255)) engine = ndb;

--echo Create other table
create table test.t2(a int primary key, b int) engine = ndb;

--source suite/ndb_rpl/t/ndb_conflict_info_init.inc

--echo ----------------------------------
--echo Test 1 : Basic two-way replication
--echo ----------------------------------

insert into test.t1 values (1, "Metropole");

# Here we want to verify that updates can originate at the Secondary without
# causing conflicts
# We first insert on the Primary, allow that to propagate, then update on 
# the secondary.
# This is much complicated by the fact that we run with varying number of
# extra GCI bits, from 0 to 31.  With 0 extra GCI bits, the row's GCI appears
# to be GCIhi|0xffffffff, which means that all secondary initiated changes
# will be considered in-conflict until the MaxReplicatedEpoch hits GCIhi+1|0
# To make this test stable, we use a loop to check for this condition!

--sync_slave_with_master slave
--connection slave
FLUSH LOGS;
--sync_slave_with_master master
--connection master
--echo -- Verify that Max rep epoch has been updated

--disable_query_log
--disable_result_log
SELECT @MaxRepGCIHI:=Variable_Value >> 32
FROM Information_schema.Global_Status
WHERE Variable_Name like 'Ndb_slave_max_replicated_epoch';

SELECT @MaxRepGCIHI = 0;

--let $first_gcihi = `SELECT @MaxRepGCIHI`
--let $current_gcihi = $first_gcihi
--let $different = `SELECT $current_gcihi - $first_gcihi`

#--echo Difference is $different

while (!$different)
{
  # Do something, sleep a little, and see if we've ticked over GCIhi yet
  replace into test.t2 values(2,1);
  replace into test.t2 values(2,2);
  --sleep 1

  SELECT @MaxRepGCIHI:=Variable_Value >> 32
  FROM Information_schema.Global_Status
  WHERE Variable_Name like 'Ndb_slave_max_replicated_epoch';

  --let $current_gcihi = `SELECT @MaxRepGCIHI`
  --let $different = `SELECT $current_gcihi - $first_gcihi`
  #--echo Current GCI Hi is $current_gcihi
  #--echo Difference is $different
}
--enable_result_log
--enable_query_log

--echo -- Primary Cluster Max Rep Epoch now beyond the original update epoch

--connection slave

--echo -- Now update data on slave
update test.t1 set b="Favorit" where a=1;

--sync_slave_with_master master

--connection master
--echo -- Now check data on master
select * from test.t1 order by a;

--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection slave
--echo -- Now perform multiple consecutive updates on the slave

update test.t1 set b="Elephant house" where a=1;
update test.t1 set b="Beach house" where a=1;

select * from test.t1 order by a;

--sync_slave_with_master master
--connection master
--echo -- Now check they've applied on the master
select * from test.t1 order by a;

--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo --------------------------------------------
--echo Test 2 : Normal Insert from Secondary Master
--echo --------------------------------------------

--connection slave
--echo -- Insert a new row on the Slave
insert into test.t1 values (2, "Forrest");

--sync_slave_with_master master
--connection master
--echo -- Check it exists on the Master
select * from test.t1 order by a;

--connection slave
--echo -- Update from the slave
update test.t1 set b="Reds" where a=2;

--sync_slave_with_master master
--connection master
select * from test.t1 order by a;

--connection slave
delete from test.t1 where a=2;

--sync_slave_with_master master
--connection master
select * from test.t1 order by a;

--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo -------------------------------
--echo Test 3 : Insert-Insert conflict
--echo -------------------------------

--connection slave
stop slave;
--connection master

--echo -- Insert a row on the Primary Master
insert into test.t1 values (2, "Loopy Lornas");

--connection slave
--echo -- Insert a row on the secondary Master
insert into test.t1 values (2, "Cloisters");

--sync_slave_with_master master
--connection master
--echo -- Examine data on Primary Master (should be unaffected)
select * from test.t1 order by a;

--echo -- Examine conflict indicators on Primary Master
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo -- Examine data on isolated secondary Master (should be as-set)
--connection slave
select * from test.t1 order by a;

--echo -- Restart secondary Masters slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave

--echo -- Reexamine secondary Master's data (should be same as Primary Masters)
select * from test.t1 order by a;

--echo -------------------------------
--echo Test 4 : Update-Update conflict
--echo -------------------------------

--connection slave
--echo -- Stop replication to secondary master
stop slave;

--connection master
--echo -- Update row on Primary Master

update test.t1 set b="Peters Yard" where a=2;

--echo -- Show data on Primary Master

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection slave

--echo -- Update row on Secondary Master
update test.t1 set b="Toast" where a=2;

--sync_slave_with_master master

--echo -- Examine data on Primary Master - should be unaffected

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection slave
--echo -- Check data on secondary - should be as set

select * from test.t1 order by a;

--echo -- Now restart slave, will re-align row
start slave;

--connection master
--sync_slave_with_master slave

--connection slave
--echo -- Check that Secondary is re-aligned

select * from test.t1 order by a;


--echo -------------------------------
--echo Test 5 : Update-Delete conflict
--echo -------------------------------

--connection slave
--echo -- Stop Secondary slave
stop slave;

--connection master
--echo -- Update on Primary Master

update test.t1 set b="Pear tree" where a = 2;

--connection slave
--echo -- Delete on Secondary Master

delete from test.t1 where a = 2;

--sync_slave_with_master master

--echo -- Examine data on Primary Master, should be unaffected

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo -- Examine data on Secondary Master before slave restart, still missing
--connection slave
select * from test.t1 order by a;

--echo -- Restart Secondary Master slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave

--echo -- Examine data on Secondary Master after slave restart, aligned with Master
select * from test.t1 order by a;

--echo -------------------------------
--echo Test 6 : Delete-Update conflict
--echo -------------------------------

--connection slave
--echo -- Stop Secondary slave
stop slave;

--connection master
--echo -- Delete on Primary Master

delete from test.t1 where a=2;

--connection slave
--echo -- Update on Secondary Master

update test.t1 set b="Black pig" where a=2;

--sync_slave_with_master master

--echo -- Examine data on Primary Master, should be unaffected (no row)

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo -- Examine data on Secondary Master before slave restart, should be as inserted
--connection slave
select * from test.t1 order by a;

--echo -- Restart Secondary Master slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave

--echo -- Examine data on Secondary Master after slave restart, aligned with Master (deleted)
select * from test.t1 order by a;

--echo -------------------------------
--echo Test 7 : Delete-Delete conflict
--echo -------------------------------

--connection slave
--echo -- Stop Secondary slave
stop slave;

--connection master
--echo -- Delete on Primary Master

delete from test.t1 where a=1;

--connection slave
--echo -- Delete on Secondary Master

delete from test.t1 where a=1;

--sync_slave_with_master master

--echo -- Examine data on Primary Master, no row

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo -- Examine data on Secondary Master before slave restart, no row
--connection slave
select * from test.t1 order by a;

--echo -- Restart Secondary Master slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave

--echo -- Examine data on Secondary Master after slave restart, no row
select * from test.t1 order by a;


--echo ------------------------------------------------
--echo Test 8 : Delete-Delete, Insert conflict exposure
--echo ------------------------------------------------
# This occurs as the Primary Master's Delete is still
# in-flight when the Secondary Master's Insert arrives,
# but as there is no knowledge of this at the Primary
# Master, it accepts the Insert.
#

--connection slave
--echo -- Insert a row on Secondary Master
insert into test.t1 values (3, "Espy");
--sync_slave_with_master master

--connection master
--echo -- Check it's present on Primary Master
select * from test.t1 order by a;

--echo -- Stop replication in both directions
stop slave;

--connection slave
stop slave;

--echo -- Delete row from both clusters
delete from test.t1 where a=3;

--connection master
delete from test.t1 where a=3;

--echo -- Follow up with Insert from Secondary master
--connection slave

insert into test.t1 values (3, "Dalriada");

--echo -- Restart replication in both directions
start slave;

--connection master
start slave;

--sync_slave_with_master slave
--connection slave
--sync_slave_with_master master

--connection slave
--echo -- Check data on both sites - diverged
--echo -- Secondary master :
select * from test.t1 order by a;
--echo -- Primary master :
--connection master
select * from test.t1 order by a;

--echo --Remove extra row
delete from test.t1 where a=3;

--echo -- Note that Delete-Delete conflict detected below
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--echo ------------------------------------------------
--echo Test 9 : Insert, Insert-Update-Delete conflict
--echo ------------------------------------------------

--connection slave
--echo -- Stop replication on Secondary Master
stop slave;

--connection master
--echo -- Insert row on Primary Master
insert into test.t1 values (4, "Haymarket");

--connection slave
--echo -- Insert row on Secondary Master
insert into test.t1 values (4, "Outhouse");
--echo -- Update row on Secondary Master
update test.t1 set b="Mathers" where a = 4;
--echo -- Delete row on Secondary Master
delete from test.t1 where a=4;

--echo -- Examine data (none) on Secondary Master
select * from test.t1 order by a;

--sync_slave_with_master master
--connection master
--echo -- Examine data on Primary Master, should be unaffected

select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection slave
--echo -- Examine data on Secondary Master (none)
select * from test.t1 order by a;

--echo -- Restart Secondary Master's slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave
--echo -- Check data on Secondary Master, should be same as Primary Master
select * from test.t1;

--echo ------------------------------------------------
--echo Test 10 : Update, Delete-Insert-Update conflict
--echo ------------------------------------------------
--connection slave
--echo -- Stop replication on Secondary Master
stop slave;

--connection master
--echo -- Update row on Primary Master
update test.t1 set b="Blind poet" where a=4;

--connection slave
--echo -- Delete row on Secondary Master
delete from test.t1 where a=4;

--echo -- Insert row on Secondary Master
insert into test.t1 values (4, "Drouthy Neebors");

--echo -- Update row on Secondary Master
update test.t1 set b="The Tankard" where a=4;

--sync_slave_with_master master

--connection master

--echo -- Check data on Primary Master, should be unaffected
select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection slave
--echo -- Check data on Secondary Master, as set

select * from test.t1 order by a;

--echo -- Restart Secondary Master slave
start slave;

--connection master
--sync_slave_with_master slave
--connection slave

--echo -- Check data on Secondary Master - should be as Primary
select * from test.t1 order by a;

--sync_slave_with_master master

--echo ------------------------------------------------------------------------
--echo Test 11 : Test Secondary insert-update-delete accepted
--echo ------------------------------------------------------------------------
--connection slave
--echo Insert row on Secondary
insert into test.t1 values (5, "Minders");
--echo Update row on Secondary
update test.t1 set b="Southsider" where a=5;
--echo Delete row on Secondary
delete from test.t1 where a=5;

--sync_slave_with_master master

--connection master
--echo Check data on Primary, should be none.  No new conflicts
select * from test.t1 order by a;
--source suite/ndb_rpl/t/ndb_conflict_info.inc

--connection master
--source suite/ndb_rpl/t/ndb_conflict_info.inc

drop table test.t1;
drop table test.t2;
drop table test.t1$EX;

--sync_slave_with_master slave

--connection master