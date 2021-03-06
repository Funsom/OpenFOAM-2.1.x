/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "displacementLayeredMotionFvMotionSolver.H"
#include "addToRunTimeSelectionTable.H"
#include "pointEdgeStructuredWalk.H"
#include "pointFields.H"
#include "PointEdgeWave.H"
#include "syncTools.H"
#include "interpolationTable.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    defineTypeNameAndDebug(displacementLayeredMotionFvMotionSolver, 0);

    addToRunTimeSelectionTable
    (
        fvMotionSolver,
        displacementLayeredMotionFvMotionSolver,
        dictionary
    );
}


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::displacementLayeredMotionFvMotionSolver::calcZoneMask
(
    const label cellZoneI,
    PackedBoolList& isZonePoint,
    PackedBoolList& isZoneEdge
) const
{
    if (cellZoneI == -1)
    {
        isZonePoint.setSize(mesh().nPoints());
        isZonePoint = 1;

        isZoneEdge.setSize(mesh().nEdges());
        isZoneEdge = 1;
    }
    else
    {
        const cellZone& cz = mesh().cellZones()[cellZoneI];

        label nPoints = 0;
        forAll(cz, i)
        {
            const labelList& cPoints = mesh().cellPoints(cz[i]);
            forAll(cPoints, cPointI)
            {
                if (!isZonePoint[cPoints[cPointI]])
                {
                    isZonePoint[cPoints[cPointI]] = 1;
                    nPoints++;
                }
            }
        }
        syncTools::syncPointList
        (
            mesh(),
            isZonePoint,
            orEqOp<unsigned int>(),
            0
        );


        // Mark edge inside cellZone
        label nEdges = 0;
        forAll(cz, i)
        {
            const labelList& cEdges = mesh().cellEdges(cz[i]);
            forAll(cEdges, cEdgeI)
            {
                if (!isZoneEdge[cEdges[cEdgeI]])
                {
                    isZoneEdge[cEdges[cEdgeI]] = 1;
                    nEdges++;
                }
            }
        }
        syncTools::syncEdgeList
        (
            mesh(),
            isZoneEdge,
            orEqOp<unsigned int>(),
            0
        );


        Info<< "On cellZone " << cz.name()
            << " marked " << returnReduce(nPoints, sumOp<label>())
            << " points and " << returnReduce(nEdges, sumOp<label>())
            << " edges." << endl;
    }
}


// Find distance to starting point
void Foam::displacementLayeredMotionFvMotionSolver::walkStructured
(
    const label cellZoneI,
    const PackedBoolList& isZonePoint,
    const PackedBoolList& isZoneEdge,
    const labelList& seedPoints,
    const vectorField& seedData,
    scalarField& distance,
    vectorField& data
) const
{
    List<pointEdgeStructuredWalk> seedInfo(seedPoints.size());

    forAll(seedPoints, i)
    {
        seedInfo[i] = pointEdgeStructuredWalk
        (
            points0()[seedPoints[i]],  // location of data
            points0()[seedPoints[i]],  // previous location
            0.0,
            seedData[i]
        );
    }

    // Current info on points
    List<pointEdgeStructuredWalk> allPointInfo(mesh().nPoints());

    // Mark points inside cellZone.
    // Note that we use points0, not mesh.points()
    // so as not to accumulate errors.
    forAll(isZonePoint, pointI)
    {
        if (isZonePoint[pointI])
        {
            allPointInfo[pointI] = pointEdgeStructuredWalk
            (
                points0()[pointI],  // location of data
                vector::max,        // not valid
                0.0,
                vector::zero        // passive data
            );
        }
    }

    // Current info on edges
    List<pointEdgeStructuredWalk> allEdgeInfo(mesh().nEdges());

    // Mark edges inside cellZone
    forAll(isZoneEdge, edgeI)
    {
        if (isZoneEdge[edgeI])
        {
            allEdgeInfo[edgeI] = pointEdgeStructuredWalk
            (
                mesh().edges()[edgeI].centre(points0()),    // location of data
                vector::max,                                // not valid
                0.0,
                vector::zero
            );
        }
    }

    // Walk
    PointEdgeWave<pointEdgeStructuredWalk> wallCalc
    (
        mesh(),
        seedPoints,
        seedInfo,

        allPointInfo,
        allEdgeInfo,
        mesh().globalData().nTotalPoints()  // max iterations
    );

    // Extract distance and passive data
    forAll(allPointInfo, pointI)
    {
        if (isZonePoint[pointI])
        {
            distance[pointI] = allPointInfo[pointI].dist();
            data[pointI] = allPointInfo[pointI].data();
        }
    }
}


// Evaluate faceZone patch
Foam::tmp<Foam::vectorField>
Foam::displacementLayeredMotionFvMotionSolver::faceZoneEvaluate
(
    const faceZone& fz,
    const labelList& meshPoints,
    const dictionary& dict,
    const PtrList<pointVectorField>& patchDisp,
    const label patchI
) const
{
    tmp<vectorField> tfld(new vectorField(meshPoints.size()));
    vectorField& fld = tfld();

    const word type(dict.lookup("type"));

    if (type == "fixedValue")
    {
        fld = vectorField("value", dict, meshPoints.size());
    }
    else if (type == "timeVaryingUniformFixedValue")
    {
        interpolationTable<vector> timeSeries(dict);

        fld = timeSeries(mesh().time().timeOutputValue());
    }
    else if (type == "slip")
    {
        if ((patchI % 2) != 1)
        {
            FatalIOErrorIn
            (
                "displacementLayeredMotionFvMotionSolver::faceZoneEvaluate(..)",
                *this
            )   << "slip can only be used on second faceZonePatch of pair."
                << "FaceZone:" << fz.name()
                << exit(FatalIOError);
        }
        // Use field set by previous bc
        fld = vectorField(patchDisp[patchI-1], meshPoints);
    }
    else if (type == "follow")
    {
        // Only on boundary faces - follow boundary conditions
        fld = vectorField(pointDisplacement_, meshPoints);
    }
    else
    {
        FatalIOErrorIn
        (
            "displacementLayeredMotionFvMotionSolver::faceZoneEvaluate(..)",
            *this
        )   << "Unknown faceZonePatch type " << type << " for faceZone "
            << fz.name() << exit(FatalIOError);
    }
    return tfld;
}


void Foam::displacementLayeredMotionFvMotionSolver::cellZoneSolve
(
    const label cellZoneI,
    const dictionary& zoneDict
)
{
    PackedBoolList isZonePoint(mesh().nPoints());
    PackedBoolList isZoneEdge(mesh().nEdges());
    calcZoneMask(cellZoneI, isZonePoint, isZoneEdge);

    const dictionary& patchesDict = zoneDict.subDict("boundaryField");

    if (patchesDict.size() != 2)
    {
        FatalIOErrorIn
        (
            "displacementLayeredMotionFvMotionSolver::"
            "correctBoundaryConditions(..)",
            *this
        )   << "Can only handle 2 faceZones (= patches) per cellZone. "
            << " cellZone:" << cellZoneI
            << " patches:" << patchesDict.toc()
            << exit(FatalIOError);
    }

    PtrList<scalarField> patchDist(patchesDict.size());
    PtrList<pointVectorField> patchDisp(patchesDict.size());

    // Allocate the fields
    label patchI = 0;
    forAllConstIter(dictionary, patchesDict, patchIter)
    {
        const word& faceZoneName = patchIter().keyword();
        label zoneI = mesh().faceZones().findZoneID(faceZoneName);
        if (zoneI == -1)
        {
            FatalIOErrorIn
            (
                "displacementLayeredMotionFvMotionSolver::"
                "correctBoundaryConditions(..)",
                *this
            )   << "Cannot find faceZone " << faceZoneName
                << endl << "Valid zones are " << mesh().faceZones().names()
                << exit(FatalIOError);
        }

        // Determine the points of the faceZone within the cellZone
        const faceZone& fz = mesh().faceZones()[zoneI];

        patchDist.set(patchI, new scalarField(mesh().nPoints()));
        patchDisp.set
        (
            patchI,
            new pointVectorField
            (
                IOobject
                (
                    mesh().cellZones()[cellZoneI].name() + "_" + fz.name(),
                    mesh().time().timeName(),
                    mesh(),
                    IOobject::NO_READ,
                    IOobject::NO_WRITE,
                    false
                ),
                pointDisplacement_  // to inherit the boundary conditions
            )
        );

        patchI++;
    }



    // 'correctBoundaryConditions'
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // Loops over all the faceZones and walks their boundary values

    // Make sure we can pick up bc values from field
    pointDisplacement_.correctBoundaryConditions();

    patchI = 0;
    forAllConstIter(dictionary, patchesDict, patchIter)
    {
        const word& faceZoneName = patchIter().keyword();
        const dictionary& faceZoneDict = patchIter().dict();

        // Determine the points of the faceZone within the cellZone
        const faceZone& fz = mesh().faceZones()[faceZoneName];
        const labelList& fzMeshPoints = fz().meshPoints();
        DynamicList<label> meshPoints(fzMeshPoints.size());
        forAll(fzMeshPoints, i)
        {
            if (isZonePoint[fzMeshPoints[i]])
            {
                meshPoints.append(fzMeshPoints[i]);
            }
        }

        // Get initial value for all the faceZone points
        tmp<vectorField> tseed = faceZoneEvaluate
        (
            fz,
            meshPoints,
            faceZoneDict,
            patchDisp,
            patchI
        );


Info<< "For cellZone:" << cellZoneI
    << " for faceZone:" << fz.name() << " nPoints:" << tseed().size()
    << " have patchField:"
    << " max:" << gMax(tseed())
    << " min:" << gMin(tseed())
    << " avg:" << gAverage(tseed())
    << endl;

        // Set distance and transported value
        walkStructured
        (
            cellZoneI,
            isZonePoint,
            isZoneEdge,

            meshPoints,
            tseed,
            patchDist[patchI],
            patchDisp[patchI]
        );

        // Implement real bc.
        patchDisp[patchI].correctBoundaryConditions();


//Info<< "Writing displacement for faceZone " << fz.name()
//    << " to " << patchDisp[patchI].name() << endl;
//patchDisp[patchI].write();

//        // Copy into pointDisplacement for other fields to use
//        forAll(isZonePoint, pointI)
//        {
//            if (isZonePoint[pointI])
//            {
//                pointDisplacement_[pointI] = patchDisp[patchI][pointI];
//            }
//        }
//        pointDisplacement_.correctBoundaryConditions();


        patchI++;
    }


    // Solve
    // ~~~~~
    // solving the interior is just interpolating

//    // Get normalised distance
//    pointScalarField distance
//    (
//        IOobject
//        (
//            "distance",
//            mesh().time().timeName(),
//            mesh(),
//            IOobject::NO_READ,
//            IOobject::NO_WRITE,
//            false
//        ),
//        pointMesh::New(mesh()),
//        dimensionedScalar("distance", dimLength, 0.0)
//    );
//    forAll(distance, pointI)
//    {
//        if (isZonePoint[pointI])
//        {
//            scalar d1 = patchDist[0][pointI];
//            scalar d2 = patchDist[1][pointI];
//            if (d1+d2 > SMALL)
//            {
//                scalar s = d1/(d1+d2);
//                distance[pointI] = s;
//            }
//        }
//    }
//    Info<< "Writing distance pointScalarField to " << mesh().time().timeName()
//        << endl;
//    distance.write();

    // Average
    forAll(pointDisplacement_, pointI)
    {
        if (isZonePoint[pointI])
        {
            scalar d1 = patchDist[0][pointI];
            scalar d2 = patchDist[1][pointI];

            scalar s = d1/(d1+d2+VSMALL);

            pointDisplacement_[pointI] =
                (1-s)*patchDisp[0][pointI]
              + s*patchDisp[1][pointI];
        }
    }
    pointDisplacement_.correctBoundaryConditions();
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::displacementLayeredMotionFvMotionSolver::
displacementLayeredMotionFvMotionSolver
(
    const polyMesh& mesh,
    Istream& is
)
:
    displacementFvMotionSolver(mesh, is),
    pointDisplacement_
    (
        IOobject
        (
            "pointDisplacement",
            fvMesh_.time().timeName(),
            fvMesh_,
            IOobject::MUST_READ,
            IOobject::AUTO_WRITE
        ),
        pointMesh::New(fvMesh_)
    )
{
    pointDisplacement_.correctBoundaryConditions();
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::displacementLayeredMotionFvMotionSolver::
~displacementLayeredMotionFvMotionSolver()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

Foam::tmp<Foam::pointField>
Foam::displacementLayeredMotionFvMotionSolver::curPoints() const
{
    tmp<pointField> tcurPoints
    (
        points0() + pointDisplacement_.internalField()
    );

    twoDCorrectPoints(tcurPoints());

//    const pointField& pts = tcurPoints();
//    forAll(pts, pointI)
//    {
//        Info<< "    from:" << mesh().points()[pointI]
//            << " to:" << pts[pointI]
//            << endl;
//    }


    return tcurPoints;
}


void Foam::displacementLayeredMotionFvMotionSolver::solve()
{
    const dictionary& ms = mesh().lookupObject<motionSolver>("dynamicMeshDict");
    const dictionary& solverDict = ms.subDict(typeName + "Coeffs");

    // Apply all regions (=cellZones)

    const dictionary& regionDicts = solverDict.subDict("regions");
    forAllConstIter(dictionary, regionDicts, regionIter)
    {
        const word& cellZoneName = regionIter().keyword();
        const dictionary& regionDict = regionIter().dict();

        label zoneI = mesh().cellZones().findZoneID(cellZoneName);

        Info<< "solve : zone:" << cellZoneName << " index:" << zoneI
            << endl;

        if (zoneI == -1)
        {
            FatalIOErrorIn
            (
                "displacementLayeredMotionFvMotionSolver::solve(..)",
                *this
            )   << "Cannot find cellZone " << cellZoneName
                << endl << "Valid zones are " << mesh().cellZones().names()
                << exit(FatalIOError);
        }

        cellZoneSolve(zoneI, regionDict);
    }
}


void Foam::displacementLayeredMotionFvMotionSolver::updateMesh
(
    const mapPolyMesh& mpm
)
{
    displacementFvMotionSolver::updateMesh(mpm);
}


// ************************************************************************* //
