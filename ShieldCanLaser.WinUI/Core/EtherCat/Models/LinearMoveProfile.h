#pragma once
#include <cmath>

namespace ecm
{
    /// <summary>
    /// Decomposes a 2-axis straight-line move into per-axis velocity (Vx, Vy) proportional to
    /// distance. Feeding each axis's own Vx/Vy as its Profile Velocity — with the SAME accel/decel
    /// TIME applied to both axes — keeps the velocity ratio constant throughout accel, cruise and
    /// decel, so the path traced by two independent CiA402 pp-mode axes stays a straight line.
    /// </summary>
    struct LinearMoveProfile
    {
        double X0 = 0, Y0 = 0, X1 = 0, Y1 = 0;
        double Dx = 0, Dy = 0, L = 0;
        double Ux = 0, Uy = 0;
        double V = 0, Vx = 0, Vy = 0;

        /// <summary>Compute the vector decomposition for a move from (x0,y0) to (x1,y1) at resultant
        /// velocity v (units/s along the path).</summary>
        void Init(double x0, double y0, double x1, double y1, double v)
        {
            X0 = x0; Y0 = y0; X1 = x1; Y1 = y1; V = v;

            Dx = X1 - X0;
            Dy = Y1 - Y0;
            L = std::sqrt(Dx * Dx + Dy * Dy);

            Ux = (L > 1e-9) ? Dx / L : 0;
            Uy = (L > 1e-9) ? Dy / L : 0;

            Vx = V * Ux;
            Vy = V * Uy;
        }
    };
}
