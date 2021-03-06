SET @CGUID := 842810;
DELETE FROM `creature` WHERE `guid` BETWEEN @CGUID+0 AND @CGUID+8;
INSERT INTO `creature` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `modelid`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `spawndist`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`) VALUES
(@CGUID+0, 52111, 0, 1, 3, 0, 0, -2714.02, -3226.03, 178.872, 2.0312, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+1, 52111, 0, 1, 3, 0, 0, -2719.87, -3234.71, 178.872, 1.48221, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+2, 52111, 0, 1, 3, 0, 0, -2728.67, -3237.67, 178.872, 0.461977, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+3, 52111, 0, 1, 3, 0, 0, -2738.82, -3232.82, 178.875, 5.76027, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+4, 52111, 0, 1, 3, 0, 0, -2747.73, -3226.91, 178.878, 5.56235, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+5, 52111, 0, 1, 3, 0, 0, -2751.41, -3218.11, 178.857, 4.46437, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+6, 52111, 0, 1, 3, 0, 0, -2725.14, -3208.59, 178.862, 0.436842, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+7, 52111, 0, 1, 3, 0, 0, -2743.97, -3206.14, 178.469, 5.83803, 300, 0, 0, 42, 0, 0, 0, 0, 0),
(@CGUID+8, 52111, 0, 1, 3, 0, 0, -2711.93, -3215.51, 178.872, 3.47241, 300, 0, 0, 42, 0, 0, 0, 0, 0);