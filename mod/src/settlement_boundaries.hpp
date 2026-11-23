// Exact `Burgher` (`Cpp:SettlementLocation`, `location_category_id` in {1=Village, 2=Town})
// boundary polygons extracted from KCD2's `Data/Tables.pak` (`Libs/Tables/rpg/location.xml`)
// and `Data/Levels/*/level.pak` (`SmartAreaShape` entities with `guidLocationId`).
//
// Shared by both the mod generator (`mod/`) and the runtime app (`app/`) so live guard
// world positions and population-center resident rosters agree to the centimetre.

#ifndef KCD2_SETTLEMENT_BOUNDARIES_HPP
#define KCD2_SETTLEMENT_BOUNDARIES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace kcd2_settlements {

struct Vec2f {
    float x;
    float y;
};

struct SettlementPolygon {
    const char* level;
    const char* pop_center_id;
    const char* display_name;
    float area_m2;
    const Vec2f* vertices;
    std::size_t vertex_count;
};

struct SpawnFallback {
    const char* soul_name;
    const char* level;
    float x;
    float y;
};

inline constexpr Vec2f kPolyVerts_0[] = {{1390.09f, 985.19f}, {1378.71f, 995.92f}, {1403.39f, 1022.70f}, {1415.57f, 1009.98f}, {1400.11f, 994.46f}};
inline constexpr Vec2f kPolyVerts_1[] = {{1407.71f, 1324.77f}, {1382.57f, 1343.46f}, {1360.12f, 1317.74f}, {1382.97f, 1292.84f}};
inline constexpr Vec2f kPolyVerts_2[] = {{1356.28f, 1034.69f}, {1355.10f, 1035.26f}, {1352.14f, 1031.08f}, {1343.95f, 1035.91f}, {1340.88f, 1031.99f}, {1360.63f, 1011.59f}, {1364.70f, 1016.93f}, {1376.45f, 1008.06f}, {1373.06f, 1003.20f}, {1371.66f, 1002.03f}, {1368.96f, 1002.83f}, {1354.51f, 984.73f}, {1345.82f, 973.50f}, {1336.89f, 980.36f}, {1336.99f, 985.24f}, {1316.91f, 1000.52f}, {1305.09f, 986.09f}, {1291.66f, 996.10f}, {1302.64f, 1010.38f}, {1279.38f, 1027.72f}, {1274.68f, 1021.18f}, {1266.27f, 1027.45f}, {1282.93f, 1050.80f}, {1279.20f, 1054.64f}, {1282.89f, 1059.48f}, {1287.40f, 1056.32f}, {1304.11f, 1078.71f}, {1318.66f, 1098.14f}, {1355.16f, 1071.66f}, {1358.39f, 1075.79f}, {1375.73f, 1063.06f}, {1372.88f, 1058.31f}, {1380.11f, 1052.17f}, {1382.08f, 1048.89f}, {1383.16f, 1041.77f}, {1382.22f, 1038.20f}, {1377.64f, 1032.11f}, {1374.61f, 1030.23f}, {1369.53f, 1029.22f}, {1367.48f, 1029.00f}, {1361.26f, 1030.90f}};
inline constexpr Vec2f kPolyVerts_3[] = {{1481.79f, 1308.15f}, {1485.22f, 1422.03f}, {1445.91f, 1447.57f}, {1416.35f, 1417.33f}, {1376.47f, 1375.75f}, {1333.09f, 1330.38f}, {1320.58f, 1315.92f}, {1332.41f, 1293.29f}, {1339.53f, 1266.11f}, {1350.16f, 1248.19f}, {1367.41f, 1238.71f}, {1413.89f, 1227.05f}, {1475.64f, 1215.78f}};
inline constexpr Vec2f kPolyVerts_4[] = {{1310.57f, 1315.92f}, {1297.99f, 1321.28f}, {1283.64f, 1325.83f}, {1234.20f, 1305.92f}, {1192.99f, 1252.99f}, {1138.46f, 1177.99f}, {1112.83f, 1136.06f}, {1095.29f, 1088.85f}, {1247.05f, 952.67f}, {1324.90f, 911.14f}, {1388.50f, 898.42f}, {1455.57f, 914.92f}, {1487.56f, 1047.48f}, {1480.92f, 1220.75f}, {1455.71f, 1224.41f}, {1396.27f, 1229.23f}, {1368.06f, 1237.03f}, {1353.69f, 1245.89f}, {1342.30f, 1263.85f}, {1331.82f, 1291.27f}, {1319.15f, 1309.51f}};
inline constexpr Vec2f kPolyVerts_5[] = {{3978.71f, 748.77f}, {3953.57f, 767.46f}, {3931.12f, 741.74f}, {3953.97f, 716.84f}};
inline constexpr Vec2f kPolyVerts_6[] = {{953.08f, 1044.81f}, {983.05f, 1026.95f}, {1018.35f, 1055.38f}, {987.58f, 1085.19f}, {960.60f, 1074.64f}};
inline constexpr Vec2f kPolyVerts_7[] = {{1438.87f, 461.23f}, {1472.66f, 499.99f}, {1430.56f, 533.13f}, {1399.57f, 490.03f}};
inline constexpr Vec2f kPolyVerts_8[] = {{2163.51f, 1466.79f}, {2176.85f, 1488.88f}, {2195.33f, 1510.10f}, {2194.80f, 1552.06f}, {2172.79f, 1553.48f}, {2145.92f, 1552.13f}, {2150.04f, 1514.42f}, {2140.51f, 1495.51f}, {2143.58f, 1480.71f}};
inline constexpr Vec2f kPolyVerts_9[] = {{597.40f, 922.51f}, {610.19f, 902.24f}, {659.14f, 912.71f}, {694.98f, 904.45f}, {700.59f, 938.25f}, {660.78f, 939.11f}, {626.80f, 951.62f}, {594.28f, 945.70f}};
inline constexpr Vec2f kPolyVerts_10[] = {{2868.02f, 2936.12f}, {2872.35f, 2904.24f}, {2955.97f, 2920.51f}, {2947.04f, 2970.63f}, {2900.78f, 2957.09f}};
inline constexpr Vec2f kPolyVerts_11[] = {{1525.81f, 222.91f}, {1536.67f, 191.66f}, {1561.92f, 178.77f}, {1578.13f, 184.33f}, {1587.78f, 204.39f}, {1591.56f, 224.31f}, {1598.70f, 248.72f}, {1592.22f, 269.99f}, {1576.03f, 280.02f}, {1555.37f, 274.39f}, {1541.21f, 248.49f}};
inline constexpr Vec2f kPolyVerts_12[] = {{1948.21f, 3429.45f}, {1962.09f, 3418.16f}, {1980.42f, 3405.23f}, {1989.42f, 3391.80f}, {2014.19f, 3404.52f}, {2040.36f, 3431.82f}, {2046.28f, 3450.76f}, {2030.06f, 3470.14f}, {2005.11f, 3481.77f}, {1982.16f, 3495.00f}, {1963.32f, 3482.10f}, {1945.35f, 3459.86f}};
inline constexpr Vec2f kPolyVerts_13[] = {{2731.68f, 1175.25f}, {2704.95f, 1159.54f}, {2672.95f, 1121.81f}, {2682.06f, 1086.29f}, {2726.19f, 1073.84f}, {2752.63f, 1089.31f}, {2778.47f, 1116.66f}, {2776.83f, 1133.20f}, {2744.62f, 1164.01f}};
inline constexpr Vec2f kPolyVerts_14[] = {{416.38f, 2410.79f}, {463.76f, 2457.10f}, {461.38f, 2485.27f}, {474.45f, 2498.72f}, {487.98f, 2507.86f}, {465.21f, 2535.48f}, {443.66f, 2528.95f}, {431.58f, 2540.96f}, {400.77f, 2535.68f}, {371.33f, 2514.15f}, {350.39f, 2467.18f}, {362.86f, 2437.94f}};
inline constexpr Vec2f kPolyVerts_15[] = {{3008.67f, 1359.03f}, {3027.57f, 1383.32f}, {3050.14f, 1370.37f}, {3057.53f, 1387.09f}, {3050.17f, 1461.57f}, {3110.34f, 1468.45f}, {3134.44f, 1414.91f}, {3138.28f, 1365.51f}, {3105.97f, 1328.88f}, {3081.88f, 1310.45f}, {3034.86f, 1328.15f}};
inline constexpr Vec2f kPolyVerts_16[] = {{1821.57f, 3004.96f}, {1824.36f, 2934.01f}, {1821.09f, 2911.56f}, {1896.21f, 2905.04f}, {1924.24f, 2975.73f}, {1915.84f, 3028.61f}, {1858.44f, 3089.12f}, {1831.12f, 3091.27f}, {1797.26f, 3080.57f}, {1782.59f, 3058.98f}, {1818.04f, 3033.67f}};
inline constexpr Vec2f kPolyVerts_17[] = {{2221.24f, 1825.36f}, {2249.55f, 1865.09f}, {2270.91f, 1898.43f}, {2327.61f, 1896.54f}, {2346.44f, 1882.52f}, {2367.06f, 1852.83f}, {2380.02f, 1825.76f}, {2383.78f, 1776.59f}, {2380.48f, 1749.99f}, {2348.52f, 1747.77f}, {2298.70f, 1749.46f}, {2279.07f, 1767.49f}, {2256.90f, 1754.91f}, {2228.68f, 1778.85f}};
inline constexpr Vec2f kPolyVerts_18[] = {{1601.73f, 2075.64f}, {1621.26f, 2033.03f}, {1580.27f, 1976.32f}, {1616.79f, 1940.61f}, {1575.04f, 1902.79f}, {1495.41f, 1883.10f}, {1478.39f, 1906.11f}, {1459.51f, 1912.37f}, {1451.53f, 1926.11f}, {1453.32f, 1960.66f}, {1471.53f, 1973.54f}, {1475.96f, 1994.08f}, {1466.90f, 2001.63f}, {1480.01f, 2022.09f}, {1506.22f, 2042.23f}, {1529.70f, 2057.05f}, {1578.69f, 2087.19f}};
inline constexpr Vec2f kPolyVerts_19[] = {{4052.79f, 732.15f}, {4056.22f, 846.03f}, {4016.91f, 871.57f}, {3987.35f, 841.33f}, {3947.47f, 799.75f}, {3904.09f, 754.38f}, {3891.58f, 739.92f}, {3903.41f, 717.29f}, {3910.53f, 690.11f}, {3921.16f, 672.19f}, {3938.41f, 662.71f}, {3984.89f, 651.05f}, {4046.64f, 639.78f}};
inline constexpr Vec2f kPolyVerts_20[] = {{2630.25f, 2721.46f}, {2542.74f, 2713.73f}, {2477.66f, 2683.74f}, {2453.09f, 2643.03f}, {2471.11f, 2600.96f}, {2556.07f, 2606.82f}, {2568.15f, 2555.35f}, {2651.57f, 2546.67f}, {2680.60f, 2592.88f}};
inline constexpr Vec2f kPolyVerts_21[] = {{1631.24f, 975.03f}, {1676.20f, 942.96f}, {1738.41f, 982.99f}, {1762.96f, 1011.45f}, {1775.30f, 1042.06f}, {1787.72f, 1095.91f}, {1781.92f, 1128.79f}, {1735.86f, 1131.26f}, {1701.96f, 1126.28f}, {1671.77f, 1128.36f}, {1642.51f, 1141.03f}, {1620.13f, 1117.47f}, {1615.72f, 1077.94f}, {1599.85f, 1039.83f}, {1612.60f, 1010.82f}};
inline constexpr Vec2f kPolyVerts_22[] = {{1484.60f, 3849.23f}, {1452.11f, 3876.89f}, {1422.79f, 3882.94f}, {1393.05f, 3842.98f}, {1417.60f, 3780.64f}, {1473.98f, 3767.87f}, {1539.57f, 3768.04f}, {1594.29f, 3779.96f}, {1656.19f, 3781.55f}, {1683.89f, 3804.06f}, {1677.95f, 3891.57f}, {1645.16f, 3903.71f}, {1601.50f, 3912.93f}, {1573.17f, 3898.47f}, {1573.32f, 3873.06f}, {1537.95f, 3861.52f}};
inline constexpr Vec2f kPolyVerts_23[] = {{623.06f, 1699.80f}, {654.30f, 1619.90f}, {715.56f, 1639.20f}, {815.06f, 1621.60f}, {828.96f, 1644.88f}, {775.41f, 1694.45f}, {804.14f, 1741.78f}, {776.72f, 1842.23f}, {635.98f, 1766.99f}};
inline constexpr Vec2f kPolyVerts_24[] = {{1595.65f, 2826.83f}, {1576.22f, 2806.93f}, {1555.72f, 2767.51f}, {1543.29f, 2728.82f}, {1543.44f, 2704.09f}, {1556.13f, 2697.16f}, {1572.21f, 2701.52f}, {1603.23f, 2719.47f}, {1617.90f, 2729.02f}, {1634.51f, 2746.94f}, {1656.63f, 2736.20f}, {1646.62f, 2721.03f}, {1656.62f, 2710.40f}, {1682.06f, 2708.11f}, {1722.21f, 2713.60f}, {1758.01f, 2734.04f}, {1780.03f, 2785.28f}, {1765.75f, 2805.13f}, {1740.16f, 2831.86f}, {1717.22f, 2841.87f}, {1707.01f, 2872.91f}, {1685.75f, 2899.46f}, {1657.04f, 2900.54f}, {1641.26f, 2895.22f}, {1624.53f, 2876.00f}, {1611.92f, 2849.51f}};
inline constexpr Vec2f kPolyVerts_25[] = {{612.38f, 376.07f}, {549.03f, 348.23f}, {500.40f, 315.97f}, {495.01f, 257.49f}, {479.06f, 202.50f}, {502.06f, 164.63f}, {575.50f, 145.27f}, {685.72f, 168.07f}, {645.66f, 322.92f}};
inline constexpr Vec2f kPolyVerts_26[] = {{3524.15f, 1082.20f}, {3498.59f, 1103.03f}, {3491.18f, 1145.91f}, {3462.67f, 1159.60f}, {3425.75f, 1138.57f}, {3395.04f, 1099.91f}, {3382.40f, 1049.40f}, {3365.68f, 1034.09f}, {3371.42f, 957.20f}, {3400.04f, 926.19f}, {3499.89f, 909.94f}, {3571.60f, 929.86f}, {3578.43f, 973.06f}, {3583.33f, 1060.35f}};
inline constexpr Vec2f kPolyVerts_27[] = {{3114.73f, 2185.24f}, {3082.50f, 2098.77f}, {3142.97f, 2071.63f}, {3192.91f, 2104.43f}, {3253.37f, 2148.51f}, {3251.71f, 2192.63f}, {3313.18f, 2205.83f}, {3322.38f, 2230.02f}, {3325.73f, 2296.71f}, {3331.45f, 2329.11f}, {3330.40f, 2363.39f}, {3297.08f, 2387.46f}, {3268.82f, 2364.91f}, {3245.14f, 2375.57f}, {3206.81f, 2371.68f}, {3149.18f, 2361.50f}, {3161.67f, 2310.62f}, {3139.63f, 2301.97f}};
inline constexpr Vec2f kPolyVerts_28[] = {{728.73f, 3226.43f}, {803.70f, 3208.98f}, {802.39f, 3248.80f}, {810.74f, 3304.32f}, {828.04f, 3349.68f}, {803.48f, 3398.43f}, {715.07f, 3387.48f}, {670.01f, 3383.52f}, {641.52f, 3394.76f}, {580.57f, 3357.58f}, {532.34f, 3343.65f}, {500.28f, 3313.68f}, {470.05f, 3293.13f}, {457.02f, 3267.23f}, {455.11f, 3224.98f}, {470.49f, 3210.95f}, {495.66f, 3215.62f}, {537.49f, 3230.37f}, {555.28f, 3222.98f}, {581.11f, 3181.50f}, {597.54f, 3166.99f}, {612.07f, 3163.95f}, {635.86f, 3175.46f}, {686.17f, 3200.78f}};
inline constexpr Vec2f kPolyVerts_29[] = {{3881.57f, 739.93f}, {3869.39f, 746.14f}, {3855.39f, 751.68f}, {3804.69f, 735.27f}, {3759.88f, 685.35f}, {3700.26f, 614.33f}, {3671.77f, 574.29f}, {3650.98f, 528.42f}, {3792.87f, 381.99f}, {3867.63f, 335.12f}, {3930.19f, 318.00f}, {3998.25f, 329.78f}, {4039.41f, 459.79f}, {4044.87f, 633.10f}, {4019.97f, 638.51f}, {3961.01f, 647.46f}, {3933.42f, 657.21f}, {3919.70f, 667.05f}, {3909.59f, 685.76f}, {3901.05f, 713.85f}, {3889.68f, 732.92f}};
inline constexpr Vec2f kPolyVerts_30[] = {{2953.90f, 929.95f}, {2888.11f, 896.07f}, {2862.43f, 854.73f}, {2853.95f, 790.27f}, {2867.72f, 725.85f}, {2932.78f, 713.89f}, {2968.37f, 693.12f}, {2959.66f, 674.28f}, {3009.89f, 633.48f}, {3071.23f, 537.55f}, {3038.92f, 523.51f}, {3008.49f, 505.50f}, {2992.19f, 427.01f}, {3003.07f, 402.08f}, {3043.74f, 349.36f}, {3084.79f, 304.12f}, {3143.70f, 277.28f}, {3186.08f, 296.40f}, {3228.94f, 345.73f}, {3286.31f, 429.80f}, {3341.88f, 516.57f}, {3349.19f, 596.84f}, {3346.32f, 575.96f}, {3357.07f, 620.78f}, {3362.09f, 668.85f}, {3367.04f, 710.09f}, {3356.66f, 741.46f}, {3372.90f, 751.47f}, {3387.31f, 784.94f}, {3323.57f, 836.71f}, {3238.30f, 913.93f}, {3222.25f, 959.58f}, {3197.17f, 994.93f}, {3147.94f, 1012.95f}, {3089.89f, 1010.89f}, {3023.47f, 968.63f}};
inline constexpr Vec2f kPolyVerts_31[] = {{1450.25f, 2572.30f}, {1442.25f, 2563.92f}, {1423.43f, 2569.42f}, {1427.48f, 2587.70f}, {1447.55f, 2585.27f}};
inline constexpr Vec2f kPolyVerts_32[] = {{1108.50f, 1354.73f}, {1135.43f, 1338.92f}, {1149.82f, 1345.56f}, {1166.18f, 1382.72f}, {1157.59f, 1396.01f}, {1120.67f, 1415.30f}, {1101.87f, 1419.93f}, {1083.68f, 1434.98f}, {1101.52f, 1376.75f}};
inline constexpr Vec2f kPolyVerts_33[] = {{1636.60f, 2136.09f}, {1624.54f, 2171.08f}, {1620.16f, 2204.60f}, {1676.98f, 2219.82f}, {1689.65f, 2198.81f}, {1696.02f, 2154.65f}, {1684.08f, 2140.29f}, {1664.74f, 2137.95f}};
inline constexpr Vec2f kPolyVerts_34[] = {{2053.66f, 1382.17f}, {2057.86f, 1336.97f}, {2094.12f, 1309.59f}, {2130.66f, 1296.25f}, {2139.63f, 1308.36f}, {2134.74f, 1328.70f}, {2126.55f, 1356.96f}, {2102.27f, 1389.15f}};
inline constexpr Vec2f kPolyVerts_35[] = {{1059.71f, 1865.10f}, {1084.08f, 1887.19f}, {1099.08f, 1921.30f}, {1124.50f, 1915.14f}, {1126.69f, 1923.69f}, {1127.45f, 1939.09f}, {1113.87f, 1947.26f}, {1109.79f, 1953.44f}, {1078.91f, 1964.13f}, {1058.40f, 1941.41f}, {1016.83f, 1904.07f}};
inline constexpr Vec2f kPolyVerts_36[] = {{2416.19f, 1768.55f}, {2406.16f, 1722.51f}, {2435.94f, 1684.67f}, {2530.73f, 1715.43f}, {2508.26f, 1781.89f}};
inline constexpr Vec2f kPolyVerts_37[] = {{1985.93f, 1554.10f}, {1958.49f, 1496.84f}, {2030.97f, 1489.86f}, {2079.93f, 1510.35f}, {2086.06f, 1565.19f}, {2052.82f, 1584.16f}, {1994.88f, 1570.86f}};
inline constexpr Vec2f kPolyVerts_38[] = {{2051.52f, 2477.39f}, {2056.90f, 2496.29f}, {2057.57f, 2548.19f}, {2024.71f, 2550.59f}, {2012.81f, 2544.23f}, {2004.06f, 2569.76f}, {1987.25f, 2597.60f}, {1970.73f, 2586.88f}, {1977.57f, 2553.15f}, {1957.51f, 2547.00f}, {1959.80f, 2520.06f}, {1949.58f, 2497.75f}, {1972.39f, 2488.17f}, {1996.55f, 2474.64f}, {1999.91f, 2460.40f}, {2042.39f, 2467.53f}};
inline constexpr Vec2f kPolyVerts_39[] = {{1696.30f, 1998.57f}, {1692.52f, 1966.64f}, {1748.22f, 1925.59f}, {1764.10f, 1918.40f}, {1807.94f, 1921.18f}, {1814.17f, 1962.85f}, {1818.27f, 1978.43f}, {1826.55f, 2010.65f}, {1787.91f, 2012.31f}, {1779.89f, 1995.39f}, {1741.14f, 2005.95f}, {1720.76f, 2016.45f}, {1709.13f, 2001.53f}};
inline constexpr Vec2f kPolyVerts_40[] = {{1791.34f, 1285.57f}, {1777.26f, 1270.37f}, {1780.10f, 1252.50f}, {1822.65f, 1206.23f}, {1859.76f, 1177.68f}, {1891.75f, 1139.27f}, {1908.05f, 1101.64f}, {1939.99f, 1132.91f}, {1975.61f, 1170.21f}, {1955.44f, 1190.03f}, {1917.69f, 1210.17f}, {1882.03f, 1234.17f}, {1843.95f, 1251.58f}};
inline constexpr Vec2f kPolyVerts_41[] = {{1413.62f, 1560.32f}, {1415.99f, 1542.57f}, {1433.07f, 1516.08f}, {1439.05f, 1511.77f}, {1482.39f, 1529.80f}, {1505.25f, 1540.13f}, {1585.65f, 1566.21f}, {1561.80f, 1540.21f}, {1542.33f, 1525.61f}, {1527.49f, 1514.32f}, {1521.93f, 1508.91f}, {1554.85f, 1472.03f}, {1578.23f, 1484.07f}, {1616.76f, 1515.19f}, {1637.29f, 1530.93f}, {1651.10f, 1550.73f}, {1643.62f, 1562.39f}, {1637.32f, 1569.02f}, {1620.30f, 1587.22f}, {1611.51f, 1604.68f}, {1537.42f, 1576.91f}, {1517.14f, 1573.30f}, {1507.69f, 1610.35f}, {1465.44f, 1600.52f}, {1409.39f, 1581.43f}};
inline constexpr Vec2f kPolyVerts_42[] = {{2486.93f, 1588.56f}, {2499.07f, 1451.97f}, {2632.47f, 1493.20f}, {2602.36f, 1631.64f}};
inline constexpr Vec2f kPolyVerts_43[] = {{2218.26f, 2003.43f}, {2209.86f, 1979.91f}, {2213.76f, 1960.58f}, {2229.68f, 1947.56f}, {2295.72f, 1928.70f}, {2341.01f, 1928.50f}, {2357.76f, 1962.65f}, {2351.04f, 1971.00f}, {2341.06f, 1978.07f}, {2344.65f, 1998.15f}, {2349.03f, 2016.24f}, {2364.32f, 2007.94f}, {2378.33f, 1996.18f}, {2395.90f, 1999.03f}, {2411.60f, 2006.57f}, {2430.05f, 1998.21f}, {2432.36f, 1982.62f}, {2470.44f, 1981.22f}, {2473.40f, 1998.49f}, {2467.80f, 2048.83f}, {2432.61f, 2044.03f}, {2419.32f, 2083.03f}, {2378.18f, 2097.07f}, {2332.42f, 2093.70f}, {2288.78f, 2092.12f}, {2252.45f, 2083.65f}, {2244.29f, 2059.38f}};
inline constexpr Vec2f kPolyVerts_44[] = {{2555.87f, 2629.38f}, {2502.13f, 2668.62f}, {2459.73f, 2683.16f}, {2400.21f, 2678.56f}, {2357.44f, 2678.39f}, {2339.04f, 2644.54f}, {2334.56f, 2612.23f}, {2332.58f, 2580.81f}, {2352.83f, 2543.87f}, {2380.63f, 2508.11f}, {2451.29f, 2492.26f}, {2486.10f, 2494.09f}, {2558.74f, 2607.80f}};

inline constexpr SettlementPolygon kSettlementPolygons[] = {
    {"klaster", "location_sedleckyKlaster_dlc3", "Sedletz Monastery", 603.9f, kPolyVerts_0, 5},
    {"klaster", "location_sedleckyKlaster_dlc3", "Sedletz Monastery", 1205.9f, kPolyVerts_1, 4},
    {"klaster", "location_sedleckyKlaster_dlc3", "Sedletz Monastery", 7582.4f, kPolyVerts_2, 41},
    {"klaster", "location_sedleckyKlaster_dlc3", "Sedletz Monastery", 25159.9f, kPolyVerts_3, 13},
    {"klaster", "location_sedleckyKlaster_dlc3", "Sedletz Monastery", 110930.7f, kPolyVerts_4, 21},
    {"kutnohorsko", "location_sedleckyKlaster", "Sedletz Monastery", 1205.9f, kPolyVerts_5, 4},
    {"kutnohorsko", "location_rabstejnka", "Rabstein", 2239.5f, kPolyVerts_6, 5},
    {"kutnohorsko", "location_cimburk", "Cimburk", 2669.0f, kPolyVerts_7, 4},
    {"kutnohorsko", "location_zajezdniHostinecPritoky", "Pschitoky Wagoners' Inn", 3371.8f, kPolyVerts_8, 9},
    {"kutnohorsko", "location_staraLhota", "Old Lhota", 3605.9f, kPolyVerts_9, 8},
    {"kutnohorsko", "location_certovka", "Devil's Den", 3765.7f, kPolyVerts_10, 5},
    {"kutnohorsko", "location_danemark", "Danemark", 4946.5f, kPolyVerts_11, 11},
    {"kutnohorsko", "location_bohounovice", "Bohunowitz", 6514.3f, kPolyVerts_12, 12},
    {"kutnohorsko", "location_zajezdniHostinecStaraKutna", "King Charles Tavern", 6863.4f, kPolyVerts_13, 9},
    {"kutnohorsko", "location_vysoka", "Vysoka", 11089.4f, kPolyVerts_14, 12},
    {"kutnohorsko", "location_staraKutna", "Old Kutna", 12388.2f, kPolyVerts_15, 11},
    {"kutnohorsko", "location_opatovice", "Opatowitz", 16475.8f, kPolyVerts_16, 11},
    {"kutnohorsko", "location_pritoky", "Pschitoky", 19058.4f, kPolyVerts_17, 14},
    {"kutnohorsko", "location_miskovice", "Miskowitz", 22118.5f, kPolyVerts_18, 17},
    {"kutnohorsko", "location_sedleckyKlaster", "Sedletz Monastery", 25159.9f, kPolyVerts_19, 13},
    {"kutnohorsko", "location_horany", "Horschan", 25618.1f, kPolyVerts_20, 9},
    {"kutnohorsko", "location_bylany", "Bylany", 25886.0f, kPolyVerts_21, 15},
    {"kutnohorsko", "location_ratbor", "Raborsch", 28871.9f, kPolyVerts_22, 16},
    {"kutnohorsko", "location_mezholezy", "Mezholes", 28955.4f, kPolyVerts_23, 9},
    {"kutnohorsko", "location_zikmunduvTabor", "Sigismund's Camp", 29084.7f, kPolyVerts_24, 26},
    {"kutnohorsko", "location_malesov", "Maleshov", 32902.2f, kPolyVerts_25, 9},
    {"kutnohorsko", "location_klasterniVinice", "Monastery Vineyard", 38538.9f, kPolyVerts_26, 14},
    {"kutnohorsko", "location_grunta", "Grund", 47079.4f, kPolyVerts_27, 18},
    {"kutnohorsko", "location_suchdol", "Suchdol", 56900.0f, kPolyVerts_28, 24},
    {"kutnohorsko", "location_sedleckyKlaster", "Sedletz Monastery", 110931.4f, kPolyVerts_29, 21},
    {"kutnohorsko", "location_kutnaHora", "Kuttenberg", 234702.4f, kPolyVerts_30, 36},
    {"trosecko", "location_vidlak", "Vidlak Pond", 467.0f, kPolyVerts_31, 5},
    {"trosecko", "location_podseminskyMlyn", "Lower Semine Mill", 3932.3f, kPolyVerts_32, 9},
    {"trosecko", "location_zajezdniHostinecZelejov", "Zhelejov Wagoners' Inn", 4853.5f, kPolyVerts_33, 8},
    {"trosecko", "location_nebakovskyMlyn", "Nebakov Mill", 5157.4f, kPolyVerts_34, 8},
    {"trosecko", "location_cikanskyTabor", "Nomad Camp", 5283.8f, kPolyVerts_35, 11},
    {"trosecko", "location_zdar_east", "Rocktower Pond", 8363.9f, kPolyVerts_36, 5},
    {"trosecko", "location_zdar_west", "Rocktower Pond", 8476.5f, kPolyVerts_37, 7},
    {"trosecko", "location_tachov", "Tachov", 8713.2f, kPolyVerts_38, 16},
    {"trosecko", "location_zelejov", "Zhelejov", 9051.5f, kPolyVerts_39, 13},
    {"trosecko", "location_tvrzNebakov", "Nebakov Fortress", 12598.8f, kPolyVerts_40, 13},
    {"trosecko", "location_semin", "Semine", 16775.4f, kPolyVerts_41, 25},
    {"trosecko", "location_slatejov", "Slatego", 17999.5f, kPolyVerts_42, 4},
    {"trosecko", "location_troskovice", "Troskowitz", 28352.2f, kPolyVerts_43, 27},
    {"trosecko", "location_hradTrosky", "Trosky Castle", 32479.7f, kPolyVerts_44, 13},
};
inline constexpr std::size_t kSettlementPolygonCount = 45;

inline constexpr SpawnFallback kLevelSpawnFallbacks[] = {
    {"kcer_brabantSoldier_1", "kutnohorsko", 2886.85f, 2926.65f},
    {"kcer_brabantSoldier_2", "kutnohorsko", 2886.02f, 2926.51f},
    {"kcer_brabantSoldier_3", "kutnohorsko", 2888.95f, 2926.70f},
    {"kcer_brabantSoldier_4", "kutnohorsko", 2888.29f, 2926.65f},
    {"kcer_brabantSoldier_5", "kutnohorsko", 2887.59f, 2926.70f},
    {"kcer_woman_4", "kutnohorsko", 2939.10f, 2879.10f},
    {"kcer_woman_5", "kutnohorsko", 2938.19f, 2878.99f},
    {"kcer_woman_6", "kutnohorsko", 2937.21f, 2878.79f},
    {"kcer_woman_7", "kutnohorsko", 2936.14f, 2878.68f},
    {"kcer_woman_8", "kutnohorsko", 2963.50f, 2883.65f},
    {"kgru_bathmaid_1", "kutnohorsko", 3207.82f, 2167.72f},
    {"kgru_bathmaid_2", "kutnohorsko", 3210.27f, 2166.73f},
    {"kgru_bathmaid_3", "kutnohorsko", 3209.12f, 2166.41f},
    {"kgru_vokrak", "kutnohorsko", 3261.61f, 2210.43f},
    {"kkut_adamZeZarici", "kutnohorsko", 3122.63f, 554.33f},
    {"kkut_additive_man_245", "kutnohorsko", 2924.63f, 630.52f},
    {"kkut_additive_man_246", "kutnohorsko", 2924.59f, 627.28f},
    {"kkut_additive_man_247", "kutnohorsko", 2924.54f, 628.38f},
    {"kkut_additive_man_248", "kutnohorsko", 2969.51f, 425.00f},
    {"kkut_additive_man_249", "kutnohorsko", 2969.52f, 427.28f},
    {"kkut_additive_man_250", "kutnohorsko", 2969.55f, 428.23f},
    {"kkut_additive_man_251", "kutnohorsko", 2969.57f, 424.05f},
    {"kkut_additive_man_252", "kutnohorsko", 2969.46f, 426.10f},
    {"kkut_additive_man_253", "kutnohorsko", 2968.41f, 425.12f},
    {"kkut_additive_man_254", "kutnohorsko", 2967.98f, 425.97f},
    {"kkut_additive_man_255", "kutnohorsko", 2968.15f, 427.28f},
    {"kkut_additive_man_256", "kutnohorsko", 2968.46f, 428.52f},
    {"kkut_additive_man_257", "kutnohorsko", 2975.08f, 403.04f},
    {"kkut_additive_man_258", "kutnohorsko", 2974.03f, 404.29f},
    {"kkut_additive_man_259", "kutnohorsko", 2975.03f, 401.86f},
    {"kkut_additive_man_260", "kutnohorsko", 2975.07f, 400.76f},
    {"kkut_additive_man_261", "kutnohorsko", 2973.71f, 403.04f},
    {"kkut_additive_man_262", "kutnohorsko", 2973.54f, 401.74f},
    {"kkut_additive_man_263", "kutnohorsko", 2973.98f, 400.89f},
    {"kkut_additive_man_264", "kutnohorsko", 2975.12f, 404.00f},
    {"kkut_additive_man_265", "kutnohorsko", 2975.14f, 399.81f},
    {"kkut_additive_man_266", "kutnohorsko", 2973.93f, 514.42f},
    {"kkut_additive_man_267", "kutnohorsko", 2972.88f, 515.67f},
    {"kkut_additive_man_268", "kutnohorsko", 2973.87f, 513.24f},
    {"kkut_additive_man_269", "kutnohorsko", 2973.92f, 512.14f},
    {"kkut_additive_man_270", "kutnohorsko", 2972.56f, 514.42f},
    {"kkut_additive_man_271", "kutnohorsko", 2972.39f, 513.12f},
    {"kkut_additive_man_272", "kutnohorsko", 2972.82f, 512.27f},
    {"kkut_additive_man_273", "kutnohorsko", 2973.97f, 515.38f},
    {"kkut_additive_man_274", "kutnohorsko", 2973.99f, 511.19f},
    {"kkut_additive_man_275", "kutnohorsko", 2923.09f, 629.14f},
    {"kkut_additive_man_276", "kutnohorsko", 2928.11f, 628.22f},
    {"kkut_additive_man_277", "kutnohorsko", 2924.66f, 626.33f},
    {"kkut_additive_man_278", "kutnohorsko", 2924.60f, 629.56f},
    {"kkut_additive_man_279", "kutnohorsko", 2859.78f, 673.47f},
    {"kkut_additive_man_280", "kutnohorsko", 2859.83f, 676.70f},
    {"kkut_additive_man_281", "kutnohorsko", 2859.85f, 672.52f},
    {"kkut_additive_man_282", "kutnohorsko", 2859.79f, 675.75f},
    {"kkut_additive_man_283", "kutnohorsko", 2859.74f, 674.57f},
    {"kkut_additive_woman_143", "kutnohorsko", 2974.12f, 399.94f},
    {"kkut_additive_woman_144", "kutnohorsko", 2972.97f, 511.32f},
    {"kkut_additive_woman_145", "kutnohorsko", 2923.40f, 628.38f},
    {"kkut_additive_woman_146", "kutnohorsko", 2923.64f, 626.46f},
    {"kkut_additive_woman_147", "kutnohorsko", 2923.46f, 627.31f},
    {"kkut_additive_woman_148", "kutnohorsko", 2968.55f, 424.18f},
    {"kkut_additive_woman_149", "kutnohorsko", 2858.59f, 674.57f},
    {"kkut_additive_woman_150", "kutnohorsko", 2858.83f, 672.65f},
    {"kkut_additive_woman_151", "kutnohorsko", 2858.65f, 673.50f},
    {"kkut_annaZValdstejna", "kutnohorsko", 2955.44f, 954.21f},
    {"kkut_betasBathmaid_1", "kutnohorsko", 3082.10f, 465.73f},
    {"kkut_bocek", "kutnohorsko", 2956.55f, 931.15f},
    {"kkut_executioner", "kutnohorsko", 3269.49f, 367.42f},
    {"kkut_franz", "kutnohorsko", 2957.68f, 951.37f},
    {"kkut_jeronym", "kutnohorsko", 3078.60f, 591.48f},
    {"kkut_konrad", "kutnohorsko", 2956.43f, 929.12f},
    {"kkut_krasek", "kutnohorsko", 2903.22f, 764.36f},
    {"kkut_man_135", "kutnohorsko", 2999.84f, 354.66f},
    {"kkut_man_136", "kutnohorsko", 3018.18f, 332.01f},
    {"kkut_man_137", "kutnohorsko", 3009.80f, 332.90f},
    {"kkut_man_138", "kutnohorsko", 3002.92f, 322.91f},
    {"kkut_man_147", "kutnohorsko", 3000.36f, 323.49f},
    {"kkut_man_148", "kutnohorsko", 2996.18f, 307.86f},
    {"kkut_man_322", "kutnohorsko", 3270.22f, 368.17f},
    {"kkut_man_333", "kutnohorsko", 2893.38f, 665.99f},
    {"kkut_man_334", "kutnohorsko", 2893.31f, 666.94f},
    {"kkut_man_335", "kutnohorsko", 2893.26f, 668.04f},
    {"kkut_man_361", "kutnohorsko", 2818.59f, 865.64f},
    {"kkut_man_362", "kutnohorsko", 2819.45f, 866.49f},
    {"kkut_man_363", "kutnohorsko", 2827.76f, 873.79f},
    {"kkut_man_364", "kutnohorsko", 2828.61f, 874.79f},
    {"kkut_man_370", "kutnohorsko", 2791.17f, 788.41f},
    {"kkut_man_371", "kutnohorsko", 2790.81f, 787.76f},
    {"kkut_man_378", "kutnohorsko", 2832.19f, 784.80f},
    {"kkut_man_379", "kutnohorsko", 2832.52f, 783.77f},
    {"kkut_man_66", "kutnohorsko", 2832.60f, 782.67f},
    {"kkut_mandelina", "kutnohorsko", 3107.71f, 841.17f},
    {"kkut_mazak", "kutnohorsko", 3176.93f, 971.12f},
    {"kkut_mikulas", "kutnohorsko", 2941.99f, 762.28f},
    {"kkut_petr", "kutnohorsko", 2956.59f, 951.55f},
    {"kkut_pisekMan_1", "kutnohorsko", 3015.96f, 783.76f},
    {"kkut_pisekMan_2", "kutnohorsko", 3015.30f, 784.29f},
    {"kkut_pisekMan_3", "kutnohorsko", 3015.08f, 785.28f},
    {"kkut_pisekMan_4", "kutnohorsko", 3016.91f, 783.76f},
    {"kkut_pisekWoman_1", "kutnohorsko", 3014.47f, 786.05f},
    {"kkut_pisekWoman_4", "kutnohorsko", 3014.99f, 786.64f},
    {"kkut_pisekWoman_5", "kutnohorsko", 3013.96f, 787.09f},
    {"kkut_rabbi", "kutnohorsko", 3287.39f, 811.15f},
    {"kkut_rozaRuthard", "kutnohorsko", 2993.06f, 665.05f},
    {"kkut_spaAdministrator", "kutnohorsko", 3083.38f, 465.48f},
    {"kkut_vaclav", "kutnohorsko", 2958.86f, 951.38f},
    {"kkut_woman_264", "kutnohorsko", 3269.10f, 368.10f},
    {"kkut_woman_267", "kutnohorsko", 2891.21f, 665.23f},
    {"kmal_closedCastle_Malesov_1", "kutnohorsko", 470.84f, 353.65f},
    {"kmal_closedCastle_Malesov_10", "kutnohorsko", 471.51f, 355.55f},
    {"kmal_closedCastle_Malesov_11", "kutnohorsko", 470.99f, 356.03f},
    {"kmal_closedCastle_Malesov_2", "kutnohorsko", 470.21f, 354.27f},
    {"kmal_closedCastle_Malesov_3", "kutnohorsko", 469.38f, 354.71f},
    {"kmal_closedCastle_Malesov_4", "kutnohorsko", 468.60f, 355.11f},
    {"kmal_closedCastle_Malesov_5", "kutnohorsko", 471.55f, 354.47f},
    {"kmal_closedCastle_Malesov_6", "kutnohorsko", 470.92f, 354.94f},
    {"kmal_closedCastle_Malesov_7", "kutnohorsko", 470.24f, 355.36f},
    {"kmal_closedCastle_Malesov_8", "kutnohorsko", 469.51f, 355.75f},
    {"kmal_closedCastle_Malesov_9", "kutnohorsko", 472.10f, 355.15f},
    {"kmis_man_35", "kutnohorsko", 1640.69f, 1962.27f},
    {"kopa_man_39", "kutnohorsko", 1743.14f, 3018.61f},
    {"kpri_woman_17", "kutnohorsko", 2159.00f, 1482.21f},
    {"krat_baronBrabant", "kutnohorsko", 1438.07f, 3832.87f},
    {"krat_krystofOderin", "kutnohorsko", 1440.02f, 3821.96f},
    {"krat_stislav", "kutnohorsko", 1624.30f, 3837.21f},
    {"ksed_man_1", "kutnohorsko", 3884.22f, 746.89f},
    {"ksed_man_2", "kutnohorsko", 3883.05f, 746.78f},
    {"ksed_monsteryGuard_1", "kutnohorsko", 3895.41f, 731.83f},
    {"ksta_additive_man_1", "kutnohorsko", 2738.25f, 1109.17f},
    {"ksta_additive_man_12", "kutnohorsko", 2754.47f, 1087.19f},
    {"ksta_additive_man_13", "kutnohorsko", 2738.50f, 1113.42f},
    {"ksta_additive_man_14", "kutnohorsko", 2755.58f, 1085.93f},
    {"ksta_additive_man_17", "kutnohorsko", 2739.35f, 1107.60f},
    {"ksta_additive_man_18", "kutnohorsko", 2750.01f, 1085.58f},
    {"ksta_additive_man_19", "kutnohorsko", 2755.51f, 1090.28f},
    {"ksta_additive_man_20", "kutnohorsko", 2749.81f, 1098.22f},
    {"ksta_additive_man_3", "kutnohorsko", 2740.48f, 1110.90f},
    {"ksta_additive_man_4", "kutnohorsko", 2750.46f, 1092.29f},
    {"ksta_additive_man_5", "kutnohorsko", 2748.82f, 1099.20f},
    {"ksta_additive_woman_1", "kutnohorsko", 2741.42f, 1109.58f},
    {"ksta_additive_woman_11", "kutnohorsko", 2758.84f, 1081.86f},
    {"ksta_additive_woman_12", "kutnohorsko", 2757.65f, 1083.34f},
    {"ksta_additive_woman_14", "kutnohorsko", 2750.96f, 1096.83f},
    {"ksta_additive_woman_2", "kutnohorsko", 2740.64f, 1105.84f},
    {"ksta_additive_woman_3", "kutnohorsko", 2749.11f, 1093.57f},
    {"ksta_additive_woman_4", "kutnohorsko", 2753.47f, 1081.87f},
    {"ksta_additive_woman_5", "kutnohorsko", 2756.51f, 1084.74f},
    {"ksta_additive_woman_6", "kutnohorsko", 2748.01f, 1094.82f},
    {"ksta_additive_woman_7", "kutnohorsko", 2746.55f, 1096.23f},
    {"ksta_additive_woman_8", "kutnohorsko", 2737.10f, 1110.26f},
    {"ksuc_jost", "kutnohorsko", 751.92f, 3347.20f},
    {"ksuc_nomad", "trosecko", 2651.15f, 1515.08f},
    {"kvrc_millersHelper_3", "kutnohorsko", 1573.55f, 218.33f},
    {"kvrc_millersSon", "kutnohorsko", 1571.55f, 217.54f},
    {"kzik_basan", "kutnohorsko", 1704.83f, 2752.80f},
    {"kzik_mysek", "kutnohorsko", 1632.10f, 2782.18f},
    {"kzik_stibor", "kutnohorsko", 1588.55f, 2764.82f},
    {"ssed_blazena", "klaster", 1395.26f, 1001.99f},
    {"ssed_blazenasFriend", "klaster", 1319.39f, 1127.04f},
    {"ssed_knightLeader_1", "klaster", 1181.46f, 1121.86f},
    {"ssed_woman_1", "klaster", 1352.95f, 1109.73f},
    {"ssed_woman_2", "klaster", 1353.40f, 1109.01f},
    {"tneb_bohuta", "kutnohorsko", 2906.61f, 2913.89f},
    {"tneb_klara", "trosecko", 1873.45f, 1221.99f},
    {"tneb_man_2", "trosecko", 1933.50f, 1167.38f},
    {"tneb_man_21", "trosecko", 1934.50f, 1167.38f},
    {"tneb_man_25", "trosecko", 2074.39f, 1267.05f},
    {"tneb_man_26", "trosecko", 2073.54f, 1266.83f},
    {"tneb_miller", "trosecko", 2112.27f, 1332.64f},
    {"tsem_hanus", "kutnohorsko", 781.11f, 3297.55f},
    {"tsem_hanus", "trosecko", 2367.28f, 2040.25f},
    {"tsem_racek", "kutnohorsko", 782.08f, 3295.56f},
    {"tsem_racek", "trosecko", 2366.10f, 2040.49f},
    {"tsem_seminjr", "trosecko", 1615.30f, 1546.04f},
    {"tsla_nomad", "trosecko", 2657.20f, 1532.73f},
    {"tsla_nomadHelper_1", "trosecko", 2656.24f, 1532.66f},
    {"tsla_nomadHelper_2", "trosecko", 2655.90f, 1531.77f},
    {"tsla_nomadWife", "trosecko", 2656.92f, 1531.70f},
    {"ttro_baba", "trosecko", 2432.00f, 2635.41f},
    {"ttro_bergov", "trosecko", 2448.30f, 2615.94f},
    {"ttro_closedCastle_trosky_1", "trosecko", 2508.96f, 2643.19f},
    {"ttro_closedCastle_trosky_10", "trosecko", 2518.72f, 2632.44f},
    {"ttro_closedCastle_trosky_11", "trosecko", 2519.61f, 2630.99f},
    {"ttro_closedCastle_trosky_12", "trosecko", 2397.72f, 2614.97f},
    {"ttro_closedCastle_trosky_13", "trosecko", 2399.29f, 2615.70f},
    {"ttro_closedCastle_trosky_14", "trosecko", 2400.97f, 2616.88f},
    {"ttro_closedCastle_trosky_15", "trosecko", 2402.13f, 2617.91f},
    {"ttro_closedCastle_trosky_2", "trosecko", 2510.10f, 2642.63f},
    {"ttro_closedCastle_trosky_3", "trosecko", 2511.43f, 2641.48f},
    {"ttro_closedCastle_trosky_4", "trosecko", 2512.58f, 2640.21f},
    {"ttro_closedCastle_trosky_5", "trosecko", 2513.50f, 2638.92f},
    {"ttro_closedCastle_trosky_6", "trosecko", 2514.36f, 2637.84f},
    {"ttro_closedCastle_trosky_7", "trosecko", 2515.36f, 2636.75f},
    {"ttro_closedCastle_trosky_8", "trosecko", 2516.51f, 2635.18f},
    {"ttro_closedCastle_trosky_9", "trosecko", 2517.60f, 2633.72f},
    {"ttro_erik", "kutnohorsko", 1588.33f, 2771.67f},
    {"ttro_hasek", "trosecko", 2458.91f, 2626.72f},
    {"ttro_herman", "trosecko", 2384.37f, 2562.24f},
    {"ttro_jitka", "trosecko", 2388.10f, 2589.02f},
    {"ttro_komori", "trosecko", 2390.38f, 2591.53f},
    {"ttro_malir", "kutnohorsko", 3039.37f, 844.17f},
    {"ttro_man_27", "trosecko", 2511.26f, 2585.36f},
    {"ttro_man_28", "trosecko", 2511.85f, 2584.77f},
    {"ttro_man_29", "trosecko", 2510.31f, 2584.52f},
    {"ttro_man_30", "trosecko", 2508.45f, 2584.51f},
    {"ttro_man_31", "trosecko", 2526.78f, 2580.91f},
    {"ttro_man_32", "trosecko", 2524.82f, 2580.46f},
    {"ttro_man_33", "trosecko", 2522.59f, 2580.15f},
    {"ttro_man_34", "trosecko", 2521.18f, 2580.07f},
    {"ttro_man_37", "trosecko", 2388.58f, 2563.83f},
    {"ttro_man_42", "trosecko", 2506.75f, 2583.50f},
    {"ttro_man_43", "trosecko", 2399.15f, 2666.40f},
    {"ttro_man_44", "trosecko", 2399.36f, 2663.80f},
    {"ttro_man_45", "trosecko", 2503.39f, 2580.97f},
    {"ttro_man_46", "trosecko", 2504.45f, 2581.01f},
    {"ttro_man_47", "trosecko", 2505.51f, 2580.86f},
    {"ttro_man_48", "trosecko", 2367.85f, 2653.74f},
    {"ttro_man_49", "trosecko", 2368.98f, 2653.52f},
    {"ttro_man_50", "trosecko", 2370.14f, 2653.34f},
    {"ttro_man_51", "trosecko", 2371.31f, 2653.28f},
    {"ttro_man_52", "trosecko", 2381.92f, 2648.36f},
    {"ttro_man_53", "trosecko", 2381.04f, 2648.29f},
    {"ttro_man_54", "trosecko", 2380.17f, 2648.31f},
    {"ttro_man_55", "trosecko", 2379.13f, 2648.23f},
    {"ttro_panna", "trosecko", 2484.99f, 2659.91f},
    {"ttro_woman_1", "trosecko", 2516.49f, 2616.44f},
    {"ttro_woman_10", "trosecko", 2390.97f, 2591.11f},
    {"ttro_woman_11", "trosecko", 2386.53f, 2588.05f},
    {"ttro_woman_12", "trosecko", 2389.57f, 2589.48f},
    {"ttro_woman_13", "trosecko", 2383.86f, 2580.76f},
    {"ttro_woman_14", "trosecko", 2458.98f, 2627.72f},
    {"ttro_woman_15", "trosecko", 2518.12f, 2615.02f},
    {"ttro_woman_16", "trosecko", 2517.45f, 2615.51f},
    {"ttro_woman_2", "trosecko", 2402.42f, 2629.27f},
    {"ttro_woman_3", "trosecko", 2403.10f, 2629.93f},
    {"ttro_woman_4", "trosecko", 2486.30f, 2659.09f},
    {"ttro_woman_5", "trosecko", 2488.10f, 2657.76f},
    {"ttro_woman_6", "trosecko", 2487.27f, 2658.41f},
    {"ttro_woman_7", "trosecko", 2401.26f, 2627.22f},
    {"ttro_woman_8", "trosecko", 2390.17f, 2591.70f},
    {"ttro_woman_9", "trosecko", 2388.96f, 2591.47f},
    {"tvez_concubine", "trosecko", 1079.77f, 1928.89f},
    {"tvid_huntsman", "trosecko", 1416.82f, 2608.46f},
    {"tvid_huntsmansSon", "trosecko", 1416.56f, 2607.47f},
    {"tvid_huntsmansWife", "trosecko", 1417.84f, 2607.76f},
    {"tvid_man_6", "trosecko", 1415.62f, 2608.74f},
    {"tvid_man_7", "trosecko", 1415.00f, 2609.68f},
    {"tzel_david", "kutnohorsko", 3135.29f, 696.08f},
    {"tzel_man_11", "trosecko", 1605.29f, 2218.79f},
    {"tzel_michal", "kutnohorsko", 3135.94f, 695.43f},
};
inline constexpr std::size_t kLevelSpawnFallbackCount = 249;

inline bool point_in_polygon(double x, double y, const SettlementPolygon& poly) {
    bool inside = false;
    const std::size_t n = poly.vertex_count;
    if (n < 3) return false;
    std::size_t j = n - 1;
    for (std::size_t i = 0; i < n; ++i) {
        const double xi = poly.vertices[i].x;
        const double yi = poly.vertices[i].y;
        const double xj = poly.vertices[j].x;
        const double yj = poly.vertices[j].y;
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi + 1e-15) + xi)) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

inline double distance_to_polygon(double x, double y, const SettlementPolygon& poly) {
    if (point_in_polygon(x, y, poly)) return 0.0;
    double best = 1e18;
    const std::size_t n = poly.vertex_count;
    for (std::size_t i = 0; i < n; ++i) {
        const double x1 = poly.vertices[i].x;
        const double y1 = poly.vertices[i].y;
        const double x2 = poly.vertices[(i + 1) % n].x;
        const double y2 = poly.vertices[(i + 1) % n].y;
        const double dx = x2 - x1;
        const double dy = y2 - y1;
        const double l2 = dx * dx + dy * dy;
        const double t = std::clamp(((x - x1) * dx + (y - y1) * dy) / (l2 + 1e-15), 0.0, 1.0);
        best = std::min(best, std::hypot(x - (x1 + t * dx), y - (y1 + t * dy)));
    }
    return best;
}

// Finds the `Burgher` population center containing `(x, y)` on `level` (smallest
// polygon first), or within `max_gate_margin_m` when > 0 (for gate guards).
inline std::string_view find_population_center(std::string_view level, double x, double y,
                                               double max_gate_margin_m = 0.0) {
    for (std::size_t i = 0; i < kSettlementPolygonCount; ++i) {
        const SettlementPolygon& poly = kSettlementPolygons[i];
        if (level == poly.level && point_in_polygon(x, y, poly)) {
            return poly.pop_center_id;
        }
    }
    if (max_gate_margin_m > 0.0) {
        double best_d = max_gate_margin_m;
        std::string_view best_id;
        for (std::size_t i = 0; i < kSettlementPolygonCount; ++i) {
            const SettlementPolygon& poly = kSettlementPolygons[i];
            if (level != poly.level) continue;
            const double d = distance_to_polygon(x, y, poly);
            if (d <= best_d) {
                best_d = d;
                best_id = poly.pop_center_id;
            }
        }
        return best_id;
    }
    return {};
}

inline std::string_view population_center_display_name(std::string_view pop_center_id) {
    for (std::size_t i = 0; i < kSettlementPolygonCount; ++i) {
        if (pop_center_id == kSettlementPolygons[i].pop_center_id) {
            return kSettlementPolygons[i].display_name;
        }
    }
    return {};
}

inline bool is_permanent_settlement_soul(std::string_view soul) {
    if (soul.empty()) return false;
    if (soul == "henry" || soul == "kkut_henry" || soul == "kkut_zikmund" ||
        soul == "kkut_markvartAulitz" || soul == "tvez_henrysMother" ||
        soul == "zachrana_fatherOfHenry") {
        return false;
    }
    static constexpr std::string_view kTransientPrefixes[] = {
        "artefakt_", "baladaZHadru_", "bohutovaVlozka_", "bratriZCimburka_",
        "budovaniLazni_", "damaVNesnazich_", "dice_", "dobyti", "donations_",
        "duels_", "dvojityAgent_", "erik_", "finale_", "havirskyTurnaj_",
        "hladAZmar_", "kejkliri_", "kocovnickaCest_", "kralovskeStribro_",
        "kutnohorskyTurnaj_", "malir_", "malirBezBudoucnosti_", "malovanoJest_",
        "mucirna_", "oblehani", "papezskyLegat_", "pogrom_", "posledniBereVse_",
        "predaniVChramu_", "prepadeni", "prijezdNaSuchdol_", "proMistraZavet_",
        "rasuvUcen_", "rutinaAVypad_", "sMlynariNejsouZerty_", "sabotazLazni_",
        "sedmStatecnych", "sesivaniTonici_", "setkaniVRatbori", "taborOdboje_",
        "traskavePoselstvi_", "utokNa", "vezniNaTroskach_", "vyvolavani_",
        "zachrana", "zbranePanaSemina_", "zikmunduvTabor_", "zoufalaObrana",
    };
    for (std::string_view p : kTransientPrefixes) {
        if (soul.size() >= p.size() && soul.substr(0, p.size()) == p) return false;
    }
    return true;
}

}  // namespace kcd2_settlements

#endif  // KCD2_SETTLEMENT_BOUNDARIES_HPP
