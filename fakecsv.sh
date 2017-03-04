#!/bin/bash
declare -i Cnt=100000
declare -i Idx=0
if [ $# -gt 0 ]; then
	Cnt=$1
fi
echo "id,msisdn,imis,name,gender,birthday,lang,salary,addr,phone,created,updated,id2,msisdn2,imis2,name2,gender2,birthday2,lang2,salary2,addr2,phone2,created2,updated2"
while [ $Idx -lt $Cnt ]; do
	Id=`shuf -i 0-999999999 -n 1`
	Msisdn=`pwgen 16 1`
	Imsi=`pwgen 10 1`
	Name=`pwgen 8 1`
	Gender=`pwgen 7 1`
	BirthDay=`pwgen 8 1`
	Lang=`pwgen 4 1`
	Salary=`shuf -i 5000-50000 -n 1`
	Address=`pwgen 16 1`
	Phone=`shuf -i 100000000-999999999 -n 1`
	Created=`shuf -i 10000000-99999999 -n 1`
	Updated=`shuf -i 10000000-99999999 -n 1`


	Id2=`shuf -i 0-999999999 -n 1`
	Msisdn2=`pwgen 16 1`
	Imsi2=`pwgen 10 1`
	Name2=`pwgen 8 1`
	Gender2=`pwgen 7 1`
	BirthDay2=`pwgen 8 1`
	Lang2=`pwgen 4 1`
	Salary2=`shuf -i 5000-50000 -n 1`
	Address2=`pwgen 16 1`
	Phone2=`shuf -i 100000000-999999999 -n 1`
	Created2=`shuf -i 10000000-99999999 -n 1`
	Updated2=`shuf -i 10000000-99999999 -n 1`
	#Created=`awk -v min=10000000 -v max=999999999 'BEGIN{srand(); print int(min+rand()*(max-min+1))}'`
	Line="$Id,$Msisdn,$Imsi,$Name,$Gender,$BirthDay,$Lang,$Salary,$Address,$Phone,$Created,$Updated,$Id2,$Msisdn2,$Imsi2,$Name2,$Gender2,$BirthDay2,$Lang2,$Salary2,$Address2,$Phone2,$Created2,$Updated2"
	echo $Line
	Idx=$(($Idx+1))
done


