
#ifndef COVARIANCE_HPP
#define COVARIANCE_HPP

//===============================================================================
// unit used for computing covariances.
// Covariances act on Points that are auto-rescaled in order to save computation time.
// The unit aso handles Point type.
// classes:
// PointsStorage, CorrelationFunction, CovarianceParameters, Covariance, Points
//===============================================================================
//
// e.g. typical use:
//   CovarianceParameters covParams(dimension, lengthscales, variance, covFamilyString);
//   Covariance covariance(covParams);
//   Points pointsX(matrixX, covParams)
//   covariance.fillCorrMatrix(K, pointsX); // fill K with correlations matrix of X

#include <cmath> // exp, pow, sqrt...
#include "common.h"
#include "messages.h"

//========================================================================== CHOSEN_STORAGE
// choice of the storage for Points:
// important because covariance calculation is one of the most expensive part in nested Kriging algo.
// storage may affect performance (cache locality, false sharing, alignement), affect memory footprint
//         and change available optimized methods (simd, arma::dot, valarray * etc.)
// see comments at the end of the unit for inserting new storages. Available choices:
// 1: std::vector<double>, 2: CompactMatrix, 3: std::vector<arma::vec>, 4: std::vector<valarray>, 5: arma::mat

#define CHOSEN_STORAGE 4

//----------- STORAGE 1: use vector<double>
#if CHOSEN_STORAGE == 1
  #define MATRIX_STORAGE 0
  using PointsStorage = std::vector<std::vector<double> >;
  using WritablePoint = PointsStorage::value_type;
  using Point = const PointsStorage::value_type;

//----------- STORAGE 2: use CompactMatrix, allows aligned, optimised storage for simd instructions
#elif CHOSEN_STORAGE == 2
  #include "compactMatrix.h"
  #define MATRIX_STORAGE 1
  using PointsStorage = nestedKrig::CompactMatrix;
  using Point = nestedKrig::CompactMatrix::constRow_type;
  using WritablePoint = nestedKrig::CompactMatrix::writableRow_type;

//----------- STORAGE 3: use vector<arma::vec>, allows arma::dot for corr
#elif CHOSEN_STORAGE == 3
  #define MATRIX_STORAGE 0
  using PointsStorage = std::vector<arma::vec>;
  using Point = const PointsStorage::value_type;
  using WritablePoint = PointsStorage::value_type;

  //----------- STORAGE 4: use valarray, allows vector operations for distances, .sum()
#elif CHOSEN_STORAGE == 4
  #include <valarray>
  #define MATRIX_STORAGE 0
  using PointsStorage = std::vector<std::valarray<double> >;
  using Point = const PointsStorage::value_type;
  using WritablePoint = PointsStorage::value_type;

  //----------- STORAGE 5: use arma::mat, allows bounds control, compact storage, arma::dot
#elif CHOSEN_STORAGE == 5
  #define MATRIX_STORAGE 1
  using PointsStorage = arma::mat;
  using Point = const arma::subview_row<double>;
  using WritablePoint = arma::subview_row<double>;
#endif

//========================================================== Covariance header

namespace nestedKrig {

using Double = long double;
using ScaledParameters = std::vector<Double> ;


//========================================================== Tiny nuggets
// with tinyNuggetOnDiag, correlation matrix diagonal becomes
// 1.0 + factor*(smallest nugget) = 1.0 + factor * 2.22045e-016
// => good results on the inversion stability of MatrixOfOnes + Diag(nugget) that can occur in practice
// => results are better if the factor is a power of 2 for singular matrices of size up to 2*factor
// for matrices of size <=512, with factor= 256, nugget=5.68434e-014,
// max error regular case=5.68434e-014, max error singular case=5.68434e-014
// almost same results when setting diag=1 and all distances increased by nugget (tinyNuggetOffDiag)
// almost same results when combining the tinyNuggetOnDiag and tinyNuggetOffDiag
// cf. unit appendix_nuggetAnalysis.h

constexpr double tinyNuggetOnDiag = 256 * std::numeric_limits<double>::epsilon(); // 5.684...e-014
constexpr double tinyNuggetOffDiag = 0.0; // 256 * std::numeric_limits<double>::epsilon();



//========================================================== CorrelationFunction
// Correlation functions
// to be applied to a data which has been rescaled (PointsStorage)
// (in order to improve performance, lengthscales are set to 1 for rescaled data)

class CorrelationFunction {
public:
  const PointDimension d;

  CorrelationFunction(const PointDimension d) : d(d)  {
  }

  virtual double corr(const Point& x1,const Point& x2) const noexcept =0;
  virtual Double scaling_factor() const =0;
  virtual ~CorrelationFunction(){}
};

//-------------- White Noise
class CorrWhiteNoise : public CorrelationFunction {
public:
  CorrWhiteNoise(const PointDimension d) :
  CorrelationFunction(d)  {
  }

  virtual double corr(const Point& x1,const Point& x2) const noexcept override {
    double s = 0.0;
    for (PointDimension k = 0; k < d; ++k) s += std::fabs((x1[k] - x2[k]));
    if (s < 0.000000000000001) return(1.0);
    else return(0.0);
  }

  virtual Double scaling_factor() const override {
    return 1.0L;
  }
};

//-------------- Gauss
class CorrGauss : public CorrelationFunction {

public:
  CorrGauss(const PointDimension d) :
  CorrelationFunction(d)  {
  }

  virtual double corr(const Point& x1, const Point& x2) const noexcept override{
    double s = tinyNuggetOffDiag;
    // #pragma omp simd reduction (+:s) aligned(x1, x2:32)
    for (PointDimension k = 0; k < d; ++k) {
      double t = x1[k] - x2[k];
      s += t*t;
    }
    return std::exp(-s);
  }

  virtual Double scaling_factor() const override {
    return sqrtl(2.0L)/2.0L;
  }
};


//-------------- exp
class Correxp : public CorrelationFunction {
public:
  Correxp(const PointDimension d) :
  CorrelationFunction(d)  {
  }

  virtual double corr(const Point& x1, const Point& x2) const noexcept override {
    double s = tinyNuggetOffDiag;
    //#pragma omp simd  reduction (+:s)
    for (PointDimension k = 0; k < d; ++k) s += std::fabs(x1[k] - x2[k]);
    return(std::exp(-s));
  }

  virtual Double scaling_factor() const override {
    return 1.0L;
  }
};

//-------------- Matern32
class CorrMatern32 : public CorrelationFunction {
public:
  CorrMatern32(const PointDimension d) : CorrelationFunction(d)  {
  }

  virtual double corr(const Point& x1, const Point& x2) const noexcept override {
    double s = tinyNuggetOffDiag;
    double ecart = 0.;
    double prod = 1.;
    for (PointDimension k = 0; k < d; ++k) {
      ecart = std::fabs(x1[k] - x2[k]);
      s += ecart;
      prod *= (1.+ecart);
    }
    return(prod*std::exp(-s));
  }

  virtual Double scaling_factor() const override {
    return sqrtl(3.0L);
  }
};

//-------------- Matern52
class CorrMatern52 : public CorrelationFunction {
  constexpr static double oneOverThree = 1.0/3.0;
public:
  CorrMatern52(const PointDimension d) : CorrelationFunction(d)  {
  }

  virtual double corr(const Point& x1, const Point& x2) const noexcept override {
    double s = tinyNuggetOffDiag, ecart ;
    double prod = 1.0;
    for (PointDimension k = 0; k < d; ++k) {
      s += ecart = std::fabs(x1[k] - x2[k]);
      prod *= (1 + ecart + ecart*ecart*oneOverThree);
      // with fma(x,y,z)=x*y+z, slower or identical, depending on compiler options
      //prod *= std::fma(std::fma(ecart, oneOverThree, 1.0), ecart, 1.0);
    }
    return prod*std::exp(-s);
  }

  virtual Double scaling_factor() const override {
    return sqrtl(5.0L);
  }
};

//-------------- Powerexp
class CorrPowerexp : public CorrelationFunction {
public:
  const arma::vec& param;

  CorrPowerexp(const PointDimension d, const arma::vec& param) :
    CorrelationFunction(d), param(param)  {
  }

  virtual double corr(const Point& x1, const Point& x2) const noexcept override {
    double s = 0.;
    for (PointDimension k = 0; k < d; ++k) s += std::pow(std::fabs(x1[k] - x2[k]) / param[k], param[k+d]);
    return(std::exp(-s));
  }

  virtual Double scaling_factor() const override {
    return 1.0L; //TODO: Optimization still possible with param[k]
  }
};

//=========================================== CovarianceParameters
// class containing covariance parameters
// this class also do precomputations in order to fasten further covariance calculations

class CovarianceParameters {
public:
  using ScalingFactors=std::vector<Double>;

private:
  const PointDimension d;
  const arma::vec param; //no &, copy to make kernel independent

  ScalingFactors createScalingFactors() {
    ScalingFactors factors(d);
    Double scalingCorr = corrFunction->scaling_factor();
    for(PointDimension k=0;k<d;++k) factors[k] = scalingCorr/param(k);
    return factors;
  }

  CorrelationFunction* getCorrelationFunction(std::string& covType) const {
    if (covType.compare("gauss")==0) {return new CorrGauss(d);}
    else if (covType.compare("exp")==0) {return new Correxp(d);}
    else if (covType.compare("matern3_2") == 0) {return new CorrMatern32(d);}
    else if (covType.compare("matern5_2") == 0) {return new CorrMatern52(d);}
    else if (covType.compare("powexp") == 0) {return new CorrPowerexp(d, param);}
    else if (covType.compare("white_noise") == 0) {return new CorrWhiteNoise(d);}
    else {
      //screen.warning("covType wrongly written, using exponential kernel");
      return new Correxp(d);}
  }

public:
  const double variance;
  const double inverseVariance;

  const CorrelationFunction* corrFunction;
  const ScalingFactors scalingFactors;

  CovarianceParameters(const PointDimension d, const arma::vec& param, const double variance, std::string covType) :
    d(d), param(param), variance(variance), inverseVariance(1/(variance+1e-100)),
    corrFunction(getCorrelationFunction(covType)),
    scalingFactors(createScalingFactors()) {
  }

  CovarianceParameters() = delete;

  ~CovarianceParameters() {
    delete corrFunction;
  }
  //-------------- this object is not copied
  CovarianceParameters (const CovarianceParameters &) = delete;
  CovarianceParameters& operator= (const CovarianceParameters &) = delete;

  CovarianceParameters (CovarianceParameters &&) = delete;
  CovarianceParameters& operator= (CovarianceParameters &&) = delete;

};
//======================================================== Points

class Points {
public:
  using Writable_Point_type = WritablePoint;
  using ReadOnly_Point_type = const Point;

private:
  PointsStorage _data{};
  PointDimension _d = 0;

  void fillWith(const arma::mat& source, const CovarianceParameters& covParam, const arma::rowvec& origin) {
    const Long nrows= source.n_rows;
    _d= source.n_cols;
    reserve(nrows, d);
    const CovarianceParameters::ScalingFactors& scalingFactors = covParam.scalingFactors;
    for(Long obs=0;obs<nrows;++obs) {
      for(PointDimension k=0;k<_d;++k)
        cell(obs,k) = (source.at(obs,k)-origin.at(k))*scalingFactors[k];
    }
  }

public:
  const PointDimension& d=_d;
  Points(const arma::mat& source, const CovarianceParameters& covParam, const arma::rowvec origin) {
    fillWith(source, covParam, origin);
  }
  Points(const arma::mat& source, const CovarianceParameters& covParam) {
    fillWith(source, covParam, arma::zeros<arma::rowvec>(source.n_cols));
  }

  Points() { } //used in splitter to obtain std::vector<Points>

#if (MATRIX_STORAGE==1)

  inline const ReadOnly_Point_type operator[](const std::size_t index) const {
    return _data.row(index);
    }
  inline Writable_Point_type operator[](const std::size_t index) {
    return _data.row(index);
  }
  inline std::size_t size() const {return _data.n_rows;}

  inline void reserve(const std::size_t rows, const std::size_t cols) {
    _data.set_size(rows, cols); _d= cols; }

  inline double& cell(const std::size_t row, const std::size_t col) { return _data.row(row)[col]; }

#else

  inline const ReadOnly_Point_type& operator[](const std::size_t index) const {
    return _data[index];
  }
  inline Writable_Point_type& operator[](const std::size_t index) {
    return _data[index];
  }
  inline std::size_t size() const {return _data.size();}
  inline void resize(const std::size_t length) { _data.resize(length); }

  inline void reserve(const std::size_t rows, const std::size_t cols) {
    _data.resize(rows);
    _d= cols;
    for(Long i=0; i<rows; ++i) _data[i].resize(cols);
   }

  inline double& cell(const std::size_t row, const std::size_t col) { return _data[row][col]; }
#endif

  Points (const Points &other) = default;
  Points& operator= (const Points &other) = default;

  Points (Points &&other) = default;
  Points& operator= (Points &&other) = default;
};

//============================================================  Covariance

class Covariance {
  const CovarianceParameters& params;
  const CorrelationFunction* corrFunction;

public:
  using NuggetVector = arma::vec;
  constexpr static double diagonalValue = 1.0 + tinyNuggetOnDiag;

  Covariance(const CovarianceParameters& params) : params(params), corrFunction(params.corrFunction) {}

  void fillAllocatedDiagonal(arma::mat& matrixToFill, const NuggetVector& nugget) const noexcept {
    const Long n = matrixToFill.n_rows, nuggetSize=nugget.size();
    if (nuggetSize==0) {
      for (Long i = 0; i < n; ++i) matrixToFill.at(i,i) = diagonalValue;
      }
    else if (nuggetSize==1) {
      double diagvalueNugget = diagonalValue + nugget[0]*params.inverseVariance;
      for (Long i = 0; i < n; ++i) matrixToFill.at(i,i) = diagvalueNugget;
      }
    else if (nuggetSize==n) {
      for (Long i = 0; i < n; ++i) matrixToFill.at(i,i) = diagonalValue + nugget[i]*params.inverseVariance;
      }
    else {
      for (Long i = 0; i < n; ++i) matrixToFill.at(i,i) = diagonalValue + nugget[i%nuggetSize]*params.inverseVariance;
    }
  }

  void fillAllocatedCorrMatrix(arma::mat& matrixToFill, const Points& points, const NuggetVector& nugget) const noexcept {
    // noexcept, assume that matrixToFill is a correctly allocated square matrix of size points.size()
    fillAllocatedDiagonal(matrixToFill, nugget);
    for (Long i = 0; i < points.size(); ++i) // parallelizable
      for (Long j = 0; j < i; ++j)
        matrixToFill.at(i,j) = matrixToFill.at(j,i) = corrFunction->corr(points[i], points[j]);
  }
  void fillAllocatedCrossCorrelations(arma::mat& matrixToFill, const Points& pointsA, const Points& pointsB) const noexcept {
    // noexcept, assume that matrixToFill is a correctly allocated matrix of size pointsA.size() x pointsB.size()
    // Warning: part of critical importance for the performance of the Algo
    for (Long j = 0; j < pointsB.size(); ++j) // arma::mat is column major ordering
      for (Long i = 0; i < pointsA.size(); ++i)
        matrixToFill.at(i,j) = corrFunction->corr(pointsA[i], pointsB[j]);
  }


  void fillCorrMatrix(arma::mat& matrixToFill, const Points& points, const NuggetVector& nugget) const {
    try{
      matrixToFill.set_size(points.size(),points.size());
      fillAllocatedCorrMatrix(matrixToFill, points, nugget);
    }
    catch(const std::exception &e) {
      Screen::error("error in fillCorrMatrix", e);
      throw;
    }
  }

  void fillCrossCorrelations(arma::mat& matrixToFill, const Points& pointsA, const Points& pointsB) const {
    try{
    matrixToFill.set_size(pointsA.size(),pointsB.size());
    fillAllocatedCrossCorrelations(matrixToFill, pointsA, pointsB);
    }
    catch(const std::exception &e) {
      Screen::error("error in fillCrossCorrelations", e);
      throw;
    }
  }
};

} //end namespace nestedKrig

#endif /* COVARIANCE_HPP */

/*  Comments if new storages are required:
 *  if MATRIX_STORAGE==0: PointsStorage can be any of std::vetor<Type>
 *     then it needs read/write[], copy ctor, default ctor, resize(), size() to get the number of points
 *  if MATRIX_STORAGE==1: PointsStorage can be any of arma::mat type
 *     then it needs read/write row(), set_size(), n_cols, ..
 *  Point needs read only [] so that vector, arma::vec, double*, arma::subview_row<double> are acceptable
 *  Point is only used in read-only operations
 *  WritablePoints needs [], copy and = and resize in the case of vector storage
 */

