select if(number % 2 = 0, map(1,2,3,4), map(3,4,5,6)) from numbers(2);
select if(number % 2 = 0, materialize(map(1,2,3,4)), map(3,4,5,6)) from numbers(2);
select if(number % 2 = 0, map(3,4,5,6), materialize(map(1,2,3,4))) from numbers(2);
select if(1, map(1,2,3,4), map(3,4,5,6)) from numbers(2);
select if(0, map(1,2,3,4), map(3,4,5,6)) from numbers(2);
select if(null, map(1,2,3,4), map(3,4,5,6)) from numbers(2);
select if(1, materialize(map(1,2,3,4)), map(3,4,5,6)) from numbers(2);
select if(0, materialize(map(1,2,3,4)), map(3,4,5,6)) from numbers(2);
select if(null, materialize(map(1,2,3,4)), map(3,4,5,6)) from numbers(2);
select if(1, map(3,4,5,6), materialize(map(1,2,3,4))) from numbers(2);
select if(0, map(3,4,5,6), materialize(map(1,2,3,4))) from numbers(2);
select if(null, map(3,4,5,6), materialize(map(1,2,3,4))) from numbers(2);
select if(1, materialize(map(3,4,5,6)), materialize(map(1,2,3,4))) from numbers(2);
select if(0, materialize(map(3,4,5,6)), materialize(map(1,2,3,4))) from numbers(2);
select if(null, materialize(map(3,4,5,6)), materialize(map(1,2,3,4))) from numbers(2);
